/*
* This file is part of the PySide project.
*
* Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
*
* Contact: PySide team <contact@pyside.org>
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "dynamicqmetaobject.h"
#include "dynamicqmetaobject_p.h"
#include "pysidesignal.h"
#include "pysidesignal_p.h"
#include "pysideproperty.h"
#include "pysideproperty_p.h"
#include "pysideslot_p.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QList>
#include <QLinkedList>
#include <QObject>
#include <cstring>
#include <QDebug>
#include <QMetaMethod>
#include <shiboken.h>


#define EMPTY_META_METHOD "0()"

using namespace PySide;

enum PropertyFlags  {
    Invalid = 0x00000000,
    Readable = 0x00000001,
    Writable = 0x00000002,
    Resettable = 0x00000004,
    EnumOrFlag = 0x00000008,
    StdCppSet = 0x00000100,
//     Override = 0x00000200,
    Constant = 0x00000400,
    Final = 0x00000800,
    Designable = 0x00001000,
    ResolveDesignable = 0x00002000,
    Scriptable = 0x00004000,
    ResolveScriptable = 0x00008000,
    Stored = 0x00010000,
    ResolveStored = 0x00020000,
    Editable = 0x00040000,
    ResolveEditable = 0x00080000,
    User = 0x00100000,
    ResolveUser = 0x00200000,
    Notify = 0x00400000
};

// these values are from moc source code, generator.cpp:66
enum MethodFlags {
    AccessPrivate = 0x00,
    AccessProtected = 0x01,
    AccessPublic = 0x02,
    MethodMethod = 0x00,
    MethodSignal = 0x04,
    MethodSlot = 0x08,
    MethodConstructor = 0x0c,
    MethodCompatibility = 0x10,
    MethodCloned = 0x20,
    MethodScriptable = 0x40
};

enum MetaDataFlags {
   IsUnresolvedType = 0x80000000,
   TypeNameIndexMask = 0x7FFFFFFF
};

class DynamicQMetaObject::DynamicQMetaObjectPrivate
{
public:
    QList<MethodData> m_methods;
    QList<PropertyData> m_properties;

    QMap<QByteArray, QByteArray> m_info;
    QByteArray m_className;
    bool m_updated; // when the meta data is not update
    int m_methodOffset;
    int m_propertyOffset;
    int m_dataSize;
    int m_emptyMethod;
    int m_nullIndex;

    DynamicQMetaObjectPrivate()
        : m_updated(false), m_methodOffset(0), m_propertyOffset(0),
          m_dataSize(0), m_emptyMethod(-1), m_nullIndex(0) {}

    int createMetaData(QMetaObject* metaObj, QLinkedList<QByteArray> &strings);
    void updateMetaObject(QMetaObject* metaObj);
    void writeMethodsData(const QList<MethodData>& methods, unsigned int** data, QLinkedList<QByteArray>& strings, int* prtIndex, int nullIndex, int flags);
    void writeStringData(char *,  QLinkedList<QByteArray> &strings);
};

bool sortMethodSignalSlot(const MethodData &m1, const MethodData &m2)
{
   if (m1.methodType() == QMetaMethod::Signal)
      return m2.methodType() == QMetaMethod::Slot;
   return false;
}

static int registerString(const QByteArray& s, QLinkedList<QByteArray>& strings)
{
    int idx = 0;
    QLinkedList<QByteArray>::const_iterator it = strings.begin();
    QLinkedList<QByteArray>::const_iterator itEnd = strings.end();
    while (it != itEnd) {
        if (strcmp(*it, s) == 0)
            return idx;
        ++idx;
        ++it;
    }
    strings.append(s);
    return idx;
}

static int blobSize(QLinkedList<QByteArray> &strings)
{
   int size = strings.size() * sizeof(QByteArrayData);

   QByteArray str;
   QByteArray debug_str;
   foreach(QByteArray field, strings) {
      str.append(field);
      str.append(char(0));

      debug_str.append(field);
      debug_str.append('|');
   }
   //qDebug()<<debug_str;
   size += str.size();
   return size;
}

static int aggregateParameterCount(const QList<MethodData> &methods)
{
   int sum = 0;
   for (int i = 0; i < methods.size(); ++i)
      sum += methods.at(i).parameterCount() * 2 + 1; // nb_param*2 (type and names) +1 for return type
   return sum;
}

static void writeString(char *out, int i, const QByteArray &str,
   const int offsetOfStringdataMember, int &stringdataOffset)
{
   int size = str.size();
   qptrdiff offset = offsetOfStringdataMember + stringdataOffset
      - i * sizeof(QByteArrayData);
   const QByteArrayData data =
      Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(size, offset);

   memcpy(out + i * sizeof(QByteArrayData), &data, sizeof(QByteArrayData));

   memcpy(out + offsetOfStringdataMember + stringdataOffset, str.constData(), size);
   out[offsetOfStringdataMember + stringdataOffset + size] = '\0';

   stringdataOffset += size + 1;
}

static int qvariant_nameToType(const char* name)
{
    if (!name)
        return 0;

    if (strcmp(name, "QVariant") == 0)
        return 0xffffffff;
    if (strcmp(name, "QCString") == 0)
        return QMetaType::QByteArray;
    if (strcmp(name, "Q_LLONG") == 0)
        return QMetaType::LongLong;
    if (strcmp(name, "Q_ULLONG") == 0)
        return QMetaType::ULongLong;
    if (strcmp(name, "QIconSet") == 0)
        return QMetaType::QIcon;

    uint tp = QMetaType::type(name);
    return tp < QMetaType::User ? tp : 0;
}

/*
  Returns true if the type is a QVariant types.
*/
static bool isVariantType(const char* type)
{
    return qvariant_nameToType(type) != 0;
}

/*!
  Returns true if the type is qreal.
*/
static bool isQRealType(const char *type)
{
    return strcmp(type, "qreal") == 0;
}

uint PropertyData::flags() const
{
    const QByteArray btype(type());
    const char* typeName = btype.data();
    uint flags = Invalid;
    if (!isVariantType(typeName))
         flags |= EnumOrFlag;
    else if (!isQRealType(typeName))
        flags |= qvariant_nameToType(typeName) << 24;

    if (PySide::Property::isReadable(m_data))
        flags |= Readable;

    if (PySide::Property::isWritable(m_data))
        flags |= Writable;

    if (PySide::Property::hasReset(m_data))
        flags |= Resettable;

    if (PySide::Property::isDesignable(m_data))
        flags |= Designable;
    else
        flags |= ResolveDesignable;

    if (PySide::Property::isScriptable(m_data))
        flags |= Scriptable;
    else
        flags |= ResolveScriptable;

    if (PySide::Property::isStored(m_data))
        flags |= Stored;
    else
        flags |= ResolveStored;

    //EDITABLE
    flags |= ResolveEditable;

    if (PySide::Property::isUser(m_data))
        flags |= User;
    else
        flags |= ResolveUser;

    if (m_notifyId != -1)
        flags |= Notify;

    if (PySide::Property::isConstant(m_data))
        flags |= Constant;

    if (PySide::Property::isFinal(m_data))
        flags |= Final;

    return flags;
}

// const QByteArray with EMPTY_META_METHOD, used to save some memory
const QByteArray MethodData::m_emptySig(EMPTY_META_METHOD);

MethodData::MethodData()
    : m_signature(m_emptySig)
{
}

MethodData::MethodData(QMetaMethod::MethodType mtype, const QByteArray& signature, const QByteArray& rtype)
    : m_mtype(mtype)
    , m_signature(QMetaObject::normalizedSignature(signature.constData()))
    , m_rtype(QMetaObject::normalizedSignature(rtype.constData()))
{
}

void MethodData::clear()
{
    m_signature = m_emptySig;
    m_rtype.clear();
}

bool MethodData::isValid() const
{
    return m_signature != m_emptySig;
}

QList<QByteArray> MethodData::parameterTypes() const
{
   const char *signature = m_signature.constData();
   QList<QByteArray> list;
   while (*signature && *signature != '(')
      ++signature;
   while (*signature && *signature != ')' && *++signature != ')') {
      const char *begin = signature;
      int level = 0;
      while (*signature && (level > 0 || *signature != ',') && *signature != ')') {
         if (*signature == '<')
            ++level;
         else if (*signature == '>')
            --level;
         ++signature;
      }
      list += QByteArray(begin, signature - begin);
   }
   return list;
}

int MethodData::parameterCount() const
{
   return parameterTypes().size();
}

QByteArray MethodData::name() const
{
   return m_signature.left(qMax(m_signature.indexOf('('), 0));
}

PropertyData::PropertyData()
    : m_notifyId(0), m_data(0)
{
}

PropertyData::PropertyData(const char* name, int notifyId, PySideProperty* data)
    : m_name(name), m_notifyId(notifyId), m_data(data)
{
}

QByteArray PropertyData::type() const
{
    return QByteArray(PySide::Property::getTypeName(m_data));
}


bool PropertyData::isValid() const
{
    return !m_name.isEmpty();
}

int PropertyData::notifyId() const
{
    return m_notifyId;
}

bool PropertyData::operator==(const PropertyData& other) const
{
    return m_data == other.m_data;
}

bool PropertyData::operator==(const char* name) const
{
    return m_name == QString(name);
}


DynamicQMetaObject::DynamicQMetaObject(PyTypeObject* type, const QMetaObject* base)
    : m_d(new DynamicQMetaObjectPrivate)
{
    d.superdata = base;
    d.stringdata = NULL;
    d.data = NULL;
    d.extradata = NULL;
    d.relatedMetaObjects = NULL;
    d.static_metacall = NULL;

    m_d->m_className = QByteArray(type->tp_name).split('.').last();
    m_d->m_methodOffset = base->methodCount() - 1;
    m_d->m_propertyOffset = base->propertyCount() - 1;
    parsePythonType(type);
}

DynamicQMetaObject::DynamicQMetaObject(const char* className, const QMetaObject* metaObject)
    : m_d(new DynamicQMetaObjectPrivate)
{
    d.superdata = metaObject;
    d.stringdata = 0;
    d.data = 0;
    d.extradata = 0;
    d.relatedMetaObjects = NULL;
    d.static_metacall = NULL;

    m_d->m_className = className;
    m_d->m_methodOffset = metaObject->methodCount() - 1;
    m_d->m_propertyOffset = metaObject->propertyCount() - 1;
}

DynamicQMetaObject::~DynamicQMetaObject()
{
    free((char *)(d.stringdata));
    free(const_cast<uint*>(d.data));
    delete m_d;
}

int DynamicQMetaObject::addMethod(QMetaMethod::MethodType mtype, const char* signature, const char* type)
{
    int index = -1;
    int counter = 0;

    QList<MethodData>::iterator it = m_d->m_methods.begin();
    for (; it != m_d->m_methods.end(); ++it) {
        if ((it->signature() == signature) && (it->methodType() == mtype))
            return m_d->m_methodOffset + counter;
        else if (!it->isValid()) {
            index = counter;
        }
        counter++;
    }

    //has blank method
    if (index != -1) {
        m_d->m_methods[index] = MethodData(mtype, signature, type);
        index++;
    } else {
        m_d->m_methods << MethodData(mtype, signature, type);
        index = m_d->m_methods.size();
    }

    m_d->m_updated = false;
    return m_d->m_methodOffset + index;
}

void DynamicQMetaObject::removeMethod(QMetaMethod::MethodType mtype, uint index)
{
    const char* methodSig = method(index).methodSignature();
    QList<MethodData>::iterator it = m_d->m_methods.begin();
    for (; it != m_d->m_methods.end(); ++it) {
        if ((it->signature() == methodSig) && (it->methodType() == mtype)){
            it->clear();
            m_d->m_updated = false;
            break;
        }
    }
}

int DynamicQMetaObject::addSignal(const char* signal, const char* type)
{
    return addMethod(QMetaMethod::Signal, signal, type);
}

int DynamicQMetaObject::addSlot(const char* slot, const char* type)
{
    return addMethod(QMetaMethod::Slot, slot, type);
}

void DynamicQMetaObject::removeSlot(uint index)
{
    removeMethod(QMetaMethod::Slot, index);
}

void DynamicQMetaObject::removeSignal(uint index)
{
    removeMethod(QMetaMethod::Signal, index);
}

int DynamicQMetaObject::addProperty(const char* propertyName, PyObject* data)
{
    int index = m_d->m_properties.indexOf(propertyName);
    if (index != -1)
        return m_d->m_propertyOffset + index;

    // retrieve notifyId
    int notifyId = -1;
    PySideProperty* property = reinterpret_cast<PySideProperty*>(data);
    if (property->d->notify) {
        const char* signalNotify = PySide::Property::getNotifyName(property);
        if (signalNotify) {
            MethodData signalObject(QMetaMethod::Signal, signalNotify, "");
            notifyId = m_d->m_methods.indexOf(signalObject);
        }
    }

    //search for a empty space
    PropertyData blank;
    index = m_d->m_properties.indexOf(blank);
    if (index != -1) {
        m_d->m_properties[index] = PropertyData(propertyName, notifyId, property);
    } else {
        m_d->m_properties << PropertyData(propertyName, notifyId, property);
        index = m_d->m_properties.size();
    }
    m_d->m_updated = false;
    return  m_d->m_propertyOffset + index;
}

void DynamicQMetaObject::addInfo(const char* key, const char* value)
{
    m_d->m_info[key] = value;
}

void DynamicQMetaObject::addInfo(QMap<QByteArray, QByteArray> info)
{
    QMap<QByteArray, QByteArray>::const_iterator i = info.constBegin();
    while (i != info.constEnd()) {
        m_d->m_info[i.key()] = i.value();
        ++i;
    }
    m_d->m_updated = false;
}

const QMetaObject* DynamicQMetaObject::update() const
{
    if (!m_d->m_updated) {
        m_d->updateMetaObject(const_cast<DynamicQMetaObject*>(this));
        m_d->m_updated = true;
    }
    return this;
}

void DynamicQMetaObject::DynamicQMetaObjectPrivate::writeMethodsData(const QList<MethodData>& methods,
                                                                     unsigned int** data,
                                                                     QLinkedList<QByteArray>& strings,
                                                                     int* prtIndex,
                                                                     int nullIndex,
                                                                     int flags)
{
    int index = *prtIndex;
    int paramsIndex = index + methods.count() * 5;

    QList<MethodData>::const_iterator it = methods.begin();

    if (m_emptyMethod == -1)
        m_emptyMethod = registerString(EMPTY_META_METHOD, strings);

    for (; it != methods.end(); ++it) {
        int name_idx = 0;
        int argc = it->parameterCount();
        if (it->signature() != EMPTY_META_METHOD)
           name_idx = registerString(it->name(), strings);
        else
           name_idx = m_emptyMethod; // func name

        (*data)[index++] = name_idx;
        (*data)[index++] = argc; // argc (previously: arg name)
        (*data)[index++] = paramsIndex; //parameter index
        (*data)[index++] = nullIndex; // tags
        (*data)[index++] = flags  |  (it->methodType() == QMetaMethod::Signal ? MethodSignal : MethodSlot);

        if (it->methodType() == QMetaMethod::Signal)
           (*data)[13] += 1; //signal count

        paramsIndex += 1 + argc * 2;
    }
    *prtIndex = index;
}

void DynamicQMetaObject::parsePythonType(PyTypeObject* type)
{
    PyObject* attrs = type->tp_dict;
    PyObject* key = 0;
    PyObject* value = 0;
    Py_ssize_t pos = 0;

    typedef std::pair<const char*, PyObject*> PropPair;
    QLinkedList<PropPair> properties;

    Shiboken::AutoDecRef slotAttrName(Shiboken::String::fromCString(PYSIDE_SLOT_LIST_ATTR));

    while (PyDict_Next(attrs, &pos, &key, &value)) {
        if (Property::checkType(value)) {
            // Leave the properties to be register after signals because they may depend on notify signals
            int index = d.superdata->indexOfProperty(Shiboken::String::toCString(key));
            if (index == -1)
                properties << PropPair(Shiboken::String::toCString(key), value);
        } else if (Signal::checkType(value)) { // Register signals
            PySideSignal* data = reinterpret_cast<PySideSignal*>(value);
            const char* signalName = Shiboken::String::toCString(key);
            data->signalName = strdup(signalName);
            QByteArray sig;
            sig.reserve(128);
            for (int i = 0; i < data->signaturesSize; ++i) {
                sig = signalName;
                sig += '(';
                if (data->signatures[i])
                    sig += data->signatures[i];
                sig += ')';
                if (d.superdata->indexOfSignal(sig) == -1)
                    addSignal(sig);
            }
        } else if (PyFunction_Check(value)) { // Register slots
            if (PyObject_HasAttr(value, slotAttrName)) {
                PyObject* signatureList = PyObject_GetAttr(value, slotAttrName);
                for(Py_ssize_t i = 0, i_max = PyList_Size(signatureList); i < i_max; ++i) {
                    PyObject* signature = PyList_GET_ITEM(signatureList, i);
                    QByteArray sig(Shiboken::String::toCString(signature));
                    //slot the slot type and signature
                    QList<QByteArray> slotInfo = sig.split(' ');
                    int index = d.superdata->indexOfSlot(slotInfo[1]);
                    if (index == -1)
                        addSlot(slotInfo[1], slotInfo[0]);
                }
            }
        }
    }

    // Register properties
    foreach (PropPair propPair, properties)
        addProperty(propPair.first, propPair.second);

    
}

/*!
  Allocate the meta data table.
  Returns the index in the table corresponding to the header fields count.
*/
int DynamicQMetaObject::DynamicQMetaObjectPrivate::createMetaData(QMetaObject* metaObj, QLinkedList<QByteArray> &strings)
{
    uint n_methods = m_methods.size();
    uint n_properties = m_properties.size();
    uint n_info = m_info.size();
    uint n_signal = 0; // Signal count will be computed later..

    int header[] = {7,                  // revision (Used by moc, qmetaobjectbuilder and qdbus)
                    0,                  // class name index in m_metadata
                    n_info, 0,          // classinfo and classinfo index
                    n_methods, 0,       // method count and method list index
                    n_properties, 0,    // prop count and prop indexes
                    0, 0,               // enum count and enum index
                    0, 0,               // constructors (since revision 2)
                    0,                  // flags (since revision 3)
                    0};                 // signal count (since revision 4)

    const int HEADER_LENGHT = sizeof(header)/sizeof(int);

    m_dataSize = HEADER_LENGHT;
    m_dataSize += n_info*2;        //class info: name, value
    m_dataSize += n_methods*5;     //method: name, argc, parameters, tag, flags
    m_dataSize += n_properties*4;  //property: name, type, flags
    m_dataSize += 1;               //eod
    
    m_dataSize += aggregateParameterCount(m_methods); // types and parameter names

    uint* data = reinterpret_cast<uint*>(realloc(const_cast<uint*>(metaObj->d.data), m_dataSize * sizeof(uint)));

    Q_ASSERT(data);
    std::memcpy(data, header, sizeof(header));

    metaObj->d.data = data;

    return HEADER_LENGHT;
}

// Writes strings to string data struct.
// The struct consists of an array of QByteArrayData, followed by a char array
// containing the actual strings. This format must match the one produced by
// moc (see generator.cpp).
void DynamicQMetaObject::DynamicQMetaObjectPrivate::writeStringData(char *out,  QLinkedList<QByteArray> &strings)
{
   Q_ASSERT(!(reinterpret_cast<quintptr>(out) & (Q_ALIGNOF(QByteArrayData)-1)));

   int offsetOfStringdataMember = strings.size() * sizeof(QByteArrayData);
   int stringdataOffset = 0;
   int i = 0;
   foreach(const QByteArray& str, strings) {
      writeString(out, i, str, offsetOfStringdataMember, stringdataOffset);
      i++;
   }
}


void DynamicQMetaObject::DynamicQMetaObjectPrivate::updateMetaObject(QMetaObject* metaObj)
{
    Q_ASSERT(!m_updated);
    uint *data = const_cast<uint*>(metaObj->d.data);
    int index = 0;
    QLinkedList<QByteArray> strings;
    m_dataSize = 0;

    // Recompute the size and reallocate memory
    // index is set after the last header field
    index = createMetaData(metaObj, strings);
    data = const_cast<uint*>(metaObj->d.data);

    registerString(m_className, strings); // register class string
    m_nullIndex = registerString("", strings); // register a null string

    //write class info
    if (m_info.size()) {
        if (data[3] == 0)
            data[3] = index;

        QMap<QByteArray, QByteArray>::const_iterator i = m_info.constBegin(); //TODO: info is a hash this can fail
        while (i != m_info.constEnd()) {
            int valueIndex = registerString(i.value(), strings);
            int keyIndex = registerString(i.key(), strings);
            data[index++] = keyIndex;
            data[index++] = valueIndex;
            i++;
        }
    }

    //write properties
    if (m_properties.size()) {
        if (data[7] == 0)
            data[7] = index;

        QList<PropertyData>::const_iterator i = m_properties.constBegin();
        while(i != m_properties.constEnd()) {
            if (i->isValid()) {
                data[index++] = registerString(i->name(), strings); // name
            } else
                data[index++] = m_nullIndex;

            data[index++] = (i->isValid() ? (registerString(i->type(), strings)) :  m_nullIndex); // normalized type
            data[index++] = i->flags();
            i++;
        }

        //write properties notify
        i = m_properties.constBegin();
        while(i != m_properties.constEnd()) {
            data[index++] = i->notifyId() >= 0 ? i->notifyId() : 0; //signal notify index
            i++;
        }
    }

    //write signals/slots (signals must be written first, see indexOfMethodRelative in qmetaobject.cpp)
    qStableSort(m_methods.begin(), m_methods.end(), sortMethodSignalSlot);

    if (m_methods.size()) {
        if (data[5] == 0)
            data[5] = index;

        writeMethodsData(m_methods, &data, strings, &index, m_nullIndex, AccessPublic);
    }

    //write signal/slots parameters
    if (m_methods.size()) {
       QList<MethodData>::iterator it = m_methods.begin();
       for (; it != m_methods.end(); ++it) {
          QList<QByteArray> paramTypeNames = it->parameterTypes();
          int paramCount = paramTypeNames.size();
          for (int i = -1; i < paramCount; ++i) {
             const QByteArray &typeName = (i < 0) ? it->returnType() : paramTypeNames.at(i);
             int typeInfo;
             if (QtPrivate::isBuiltinType(typeName))
                typeInfo = QMetaType::type(typeName);
             else
                typeInfo = IsUnresolvedType | registerString(typeName, strings);
             data[index++] = typeInfo;
          }

          // Parameter names (use a null string)
          for (int i = 0; i < paramCount; ++i) {
             data[index++] = m_nullIndex;
          }
       }
    }

    data[index++] = 0; // the end

    // create the m_metadata string
    int size = blobSize(strings);
    char *blob = reinterpret_cast<char *>(realloc((char*)metaObj->d.stringdata, size));
    writeStringData(blob, strings);

    metaObj->d.stringdata = reinterpret_cast<const QByteArrayData *>(blob);
    metaObj->d.data = data;
}
