/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010 Facebook, Inc. (http://www.facebook.com)          |
   | Copyright (c) 1997-2010 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include <cpp/ext/ext_simplexml.h>
#include <cpp/ext/ext_file.h>
#include <cpp/base/class_info.h>

namespace HPHP {
///////////////////////////////////////////////////////////////////////////////

// This is to make sure each node holds one reference of m_doc, so not to let
// it go out of scope.
class XmlDocWrapper : public SweepableResourceData {
public:
  DECLARE_OBJECT_ALLOCATION(XmlDocWrapper);

  // overriding ResourceData
  const char *o_getClassName() const { return "xmlDoc";}

  XmlDocWrapper(xmlDocPtr doc) : m_doc(doc) {
  }

  ~XmlDocWrapper() {
    if (m_doc) {
      xmlFreeDoc(m_doc);
    }
  }

private:
  xmlDocPtr m_doc;
};
IMPLEMENT_OBJECT_ALLOCATION(XmlDocWrapper);

///////////////////////////////////////////////////////////////////////////////
// helpers

static inline bool match_ns(xmlNodePtr node, CStrRef ns, bool is_prefix) {
  if (ns.empty()) {
    return true;
  }
  if (node->ns == NULL || node->ns->prefix == NULL) {
    return false;
  }
  if (node->ns && !xmlStrcmp(is_prefix ? node->ns->prefix : node->ns->href,
                             (const xmlChar *)ns.data())) {
    return true;
  }
  return false;
}

static String node_list_to_string(xmlDocPtr doc, xmlNodePtr list) {
  xmlChar *tmp = xmlNodeListGetString(doc, list, 1);
  char *res = strdup((char*)tmp);
  xmlFree(tmp);
  return String((const char *)res, AttachString);
}

static Array collect_attributes(xmlNodePtr node, CStrRef ns, bool is_prefix) {
  ASSERT(node);
  Array attributes = Array::Create();
  for (xmlAttrPtr attr = node->properties; attr; attr = attr->next) {
    if (match_ns((xmlNodePtr)attr, ns, is_prefix)) {
      String n = String((char*)attr->name, xmlStrlen(attr->name), CopyString);
      attributes.set(n, node_list_to_string(node->doc, attr->children));
    }
  }
  return attributes;
}

static void add_property(Array &properties, xmlNodePtr node, Object value) {
  const char *name = (char *)node->name;
  if (name) {
    int namelen = xmlStrlen(node->name);
    String sname(name, namelen, CopyString);

    if (properties.exists(sname)) {
      Variant &existing = properties.lval(sname);
      if (existing.is(KindOfArray)) {
        existing.append(value);
      } else {
        Array newdata;
        newdata.append(existing);
        newdata.append(value);
        properties.set(sname, newdata);
      }
    } else {
      properties.set(sname, value);
    }
  }
}

static c_simplexmlelement *create_text(CObjRef doc, xmlNodePtr node,
                                       CStrRef value, CStrRef ns,
                                       bool is_prefix) {
  c_simplexmlelement *elem = NEW(c_simplexmlelement)();
  elem->m_doc = doc;
  elem->m_node = node->parent; // assign to parent, not node
  elem->ObjectData::o_set(0, -1, value);
  elem->m_attributes = collect_attributes(node->parent, ns, is_prefix);
  return elem;
}

static Array create_children(CObjRef doc, xmlNodePtr root,
                             CStrRef ns, bool is_prefix);

static c_simplexmlelement *create_element(CObjRef doc, xmlNodePtr node,
                                          CStrRef ns, bool is_prefix) {
  c_simplexmlelement *elem = NEW(c_simplexmlelement)();
  elem->m_doc = doc;
  elem->m_node = node;
  if (node) {
    elem->ObjectData::o_set(create_children(doc, node, ns, is_prefix));
    elem->m_attributes = collect_attributes(node, ns, is_prefix);
  }
  return elem;
}

static Array create_children(CObjRef doc, xmlNodePtr root,
                             CStrRef ns, bool is_prefix) {
  Array properties = Array::Create();
  for (xmlNodePtr node = root->children; node; node = node->next) {
    if (node->children || node->prev || node->next) {
      if (node->type == XML_TEXT_NODE) {
        // bad node from parser, ignoring it...
        continue;
      }
    } else {
      if (node->type == XML_TEXT_NODE) {
        if (node->content && *node->content) {
          add_property
            (properties, root,
             create_text(doc, node, node_list_to_string(root->doc, node),
                         ns, is_prefix));
        }
        continue;
      }
    }

    if (node->type != XML_ELEMENT_NODE || match_ns(node, ns, is_prefix)) {
      xmlNodePtr child = node->children;
      Object sub;
      if (child && child->type == XML_TEXT_NODE && !xmlIsBlankNode(child)) {
        sub = create_text(doc, child, node_list_to_string(root->doc, child),
                          ns, is_prefix);
      } else {
        sub = create_element(doc, node, ns, is_prefix);
      }
      add_property(properties, node, sub);
    }
  }
  return properties;
}

static inline void add_namespace_name(Array &out, xmlNsPtr ns) {
  const char *prefix = ns->prefix ? (const char *)ns->prefix : "";
  if (!out.exists(prefix)) {
    out.set(String(prefix, CopyString), String((char*)ns->href, CopyString));
  }
}

static void add_namespaces(Array &out, xmlNodePtr node, bool recursive) {
  if (node->ns) {
    add_namespace_name(out, node->ns);
  }

  for (xmlAttrPtr attr = node->properties; attr; attr = attr->next) {
    if (attr->ns) {
      add_namespace_name(out, attr->ns);
    }
  }

  if (recursive) {
    for (node = node->children; node; node = node->next) {
      if (node->type == XML_ELEMENT_NODE) {
        add_namespaces(out, node, true);
      }
    }
  }
}

static void add_registered_namespaces(Array &out, xmlNodePtr node,
                                      bool recursive) {
  if (node->type == XML_ELEMENT_NODE) {
    for (xmlNsPtr ns = node->nsDef; ns; ns = ns->next) {
      add_namespace_name(out, ns);
    }
    if (recursive) {
      for (node = node->children; node; node = node->next) {
        add_registered_namespaces(out, node, true);
      }
    }
  }
}

///////////////////////////////////////////////////////////////////////////////
// simplexml

Variant f_simplexml_load_string(CStrRef data,
                                CStrRef class_name /* = "SimpleXMLElement" */,
                                int64 options /* = 0 */,
                                CStrRef ns /* = "" */,
                                bool is_prefix /* = false */) {
  if (strcasecmp(class_name.data(), "SimpleXMLElement")) {
    if (class_name.empty()) {
      throw ArgumentMissingException("class_name");
    }
    const ClassInfo *cls = ClassInfo::FindClass(class_name);
    if (cls == NULL) {
      throw InvalidArgumentException("(class not found)", class_name.data());
    }
    if (!cls->derivesFrom("simplexmlelement", false)) {
      throw InvalidArgumentException("(not subclass of SimpleXMLElement)",
                                     class_name.data());
    }
  }

  xmlDocPtr doc = xmlReadMemory(data.data(), data.size(), NULL, NULL, options);
  if (!doc) {
    return false;
  }
  return Object(create_element(Object(NEW(XmlDocWrapper)(doc)),
                               doc->children, ns, is_prefix));
}

Variant f_simplexml_load_file(CStrRef filename,
                              CStrRef class_name /* = "SimpleXMLElement" */,
                              int64 options /* = 0 */, CStrRef ns /* = "" */,
                              bool is_prefix /* = false */) {
  String str = f_file_get_contents(filename);
  return f_simplexml_load_string(str, class_name, options, ns, is_prefix);
}

///////////////////////////////////////////////////////////////////////////////
// SimpleXMLElement

c_simplexmlelement::c_simplexmlelement()
  : m_node(NULL), m_is_attribute(false), m_is_children(false), m_xpath(NULL) {
}

c_simplexmlelement::~c_simplexmlelement() {
  if (m_xpath) {
    xmlXPathFreeContext(m_xpath);
  }
}

void c_simplexmlelement::t___construct(CStrRef data, int64 options /* = 0 */,
                                       bool data_is_url /* = false */,
                                       CStrRef ns /* = "" */,
                                       bool is_prefix /* = false */) {
  String xml = data;
  if (data_is_url) {
    Variant ret = f_file_get_contents(data);
    if (same(ret, false)) {
      Logger::Error("Unable to retrieve XML content from %s", data.data());
      return;
    }
    xml = ret.toString();
  }

  xmlDocPtr doc = xmlReadMemory(xml.data(), xml.size(), NULL, NULL, options);
  if (doc) {
    m_doc = Object(NEW(XmlDocWrapper)(doc));
    m_node = doc->children;
    if (m_node) {
      ObjectData::o_set(create_children(m_doc, m_node, ns, is_prefix));
      m_attributes = collect_attributes(m_node, ns, is_prefix);
    }
  }
}

Variant c_simplexmlelement::t_xpath(CStrRef path) {
  if (m_is_attribute || !m_node) {
    return null;
  }

  xmlDocPtr doc = m_node->doc;

  int nsnbr = 0;
  xmlNsPtr *ns = xmlGetNsList(doc, m_node);
  if (ns != NULL) {
    while (ns[nsnbr] != NULL) {
      nsnbr++;
    }
  }

  if (m_xpath == NULL) {
    m_xpath = xmlXPathNewContext(doc);
  }
  m_xpath->node = m_node;
  m_xpath->namespaces = ns;
  m_xpath->nsNr = nsnbr;

  xmlXPathObjectPtr retval = xmlXPathEval((xmlChar *)path.data(), m_xpath);
  if (ns != NULL) {
    xmlFree(ns);
    m_xpath->namespaces = NULL;
    m_xpath->nsNr = 0;
  }

  if (!retval) {
    return false;
  }

  xmlNodeSetPtr result = retval->nodesetval;
  if (!result) {
    xmlXPathFreeObject(retval);
    return false;
  }

  Array ret = Array::Create();
  for (int i = 0; i < result->nodeNr; ++i) {
    xmlNodePtr nodeptr = result->nodeTab[i];
    Object sub;
    /**
     * Detect the case where the last selector is text(), simplexml
     * always accesses the text() child by default, therefore we assign
     * to the parent node.
     */
    switch (nodeptr->type) {
    case XML_TEXT_NODE:
      sub = create_element(m_doc, nodeptr->parent, String(), false);
      break;
    case XML_ELEMENT_NODE:
      sub = create_element(m_doc, nodeptr, String(), false);
      break;
    case XML_ATTRIBUTE_NODE:
      sub = create_element(m_doc, nodeptr->parent, String(), false);
      break;
    default:
      break;
    }
    ret.append(sub);
  }

  xmlXPathFreeObject(retval);
  return ret;
}

bool c_simplexmlelement::t_registerxpathnamespace(CStrRef prefix, CStrRef ns) {
  if (m_node) {
    if (!m_xpath) {
      m_xpath = xmlXPathNewContext(m_node->doc);
    }
    return xmlXPathRegisterNs(m_xpath, (xmlChar *)prefix.data(),
                              (xmlChar *)ns.data()) == 0;
  }
  return false;
}

Variant c_simplexmlelement::t_asxml(CStrRef filename) {
  if (!m_node) return false;

  if (!filename.empty()) {
    std::string translated = File::TranslatePath(filename).data();

    if (m_node->parent && m_node->parent->type == XML_DOCUMENT_NODE) {
      int bytes = xmlSaveFile(translated.c_str(), (xmlDocPtr)m_node->doc);
      return bytes != -1;
    }

    xmlOutputBufferPtr outbuf =
      xmlOutputBufferCreateFilename(translated.c_str(), NULL, 0);
    if (outbuf == NULL) {
      return false;
    }
    xmlNodeDumpOutput(outbuf, m_node->doc, m_node, 0, 0,
                      (char*)m_node->doc->encoding);
    xmlOutputBufferClose(outbuf);
    return true;
  }

  xmlChar *strval;
  int strval_len;
  if (m_node->parent && m_node->parent->type == XML_DOCUMENT_NODE) {
    xmlDocDumpMemory(m_node->doc, &strval, &strval_len);
    String ret((char *)strval, strval_len, CopyString);
    xmlFree(strval);
    return ret;
  }

  xmlOutputBufferPtr outbuf = xmlAllocOutputBuffer(NULL);
  if (outbuf == NULL) {
    return false;
  }
  xmlNodeDumpOutput(outbuf, m_node->doc, m_node, 0, 0,
                    (char*)m_node->doc->encoding);
  xmlOutputBufferFlush(outbuf);
  String ret((char *)outbuf->buffer->content, outbuf->buffer->use, CopyString);
  xmlOutputBufferClose(outbuf);
  return ret;
}

Array c_simplexmlelement::t_getnamespaces(bool recursive /* = false */) {
  Array ret = Array::Create();
  if (m_node) {
    if (m_node->type == XML_ELEMENT_NODE) {
      add_namespaces(ret, m_node, recursive);
    } else if (m_node->type == XML_ATTRIBUTE_NODE && m_node->ns) {
      add_namespace_name(ret, m_node->ns);
    }
  }
  return ret;
}

Array c_simplexmlelement::t_getdocnamespaces(bool recursive /* = false */) {
  Array ret = Array::Create();
  if (m_node) {
    add_registered_namespaces(ret, xmlDocGetRootElement(m_node->doc),
                              recursive);
  }
  return ret;
}

Object c_simplexmlelement::t_children(CStrRef ns /* = "" */,
                                      bool is_prefix /* = false */) {
  if (m_is_attribute) {
    return Object();
  }

  c_simplexmlelement *elem = NEW(c_simplexmlelement)();
  elem->m_is_children = true;
  if (o_properties) {
    if (ns.empty()) {
      elem->ObjectData::o_set(*o_properties);
    } else {
      Array props = Array::Create();
      for (ArrayIter iter(*o_properties); iter; ++iter) {
        if (iter.second().isObject()) {
          c_simplexmlelement *elem = iter.second().toObject().
            getTyped<c_simplexmlelement>();
          if (elem->m_node && match_ns(elem->m_node, ns, is_prefix)) {
            props.set(iter.first(), iter.second());
          }
        } else {
          Array subnodes;
          for (ArrayIter iter2(iter.second()); iter2; ++iter2) {
            c_simplexmlelement *elem = iter2.second().toObject().
              getTyped<c_simplexmlelement>();
            if (elem->m_node && match_ns(elem->m_node, ns, is_prefix)) {
              subnodes.append(iter2.second());
            }
          }
          if (!subnodes.empty()) {
            if (subnodes.size() == 1) {
              props.set(iter.first(), subnodes[0]);
            } else {
              props.set(iter.first(), subnodes);
            }
          }
        }
      }
      elem->ObjectData::o_set(props);
    }
  }
  return elem;
}

String c_simplexmlelement::t_getname() {
  if (m_node) {
    int namelen = xmlStrlen(m_node->name);
    return String((char*)m_node->name, namelen, CopyString);
  }
  if (o_properties) {
    ArrayIter iter(*o_properties);
    if (iter && iter.second().isObject()) {
      return iter.second().toObject().getTyped<c_simplexmlelement>()->
        t_getname();
    }
  }
  return String();
}

Object c_simplexmlelement::t_attributes(CStrRef ns /* = "" */,
                                        bool is_prefix /* = false */) {
  if (m_is_attribute) {
    return Object();
  }

  c_simplexmlelement *elem = NEW(c_simplexmlelement)();
  elem->m_is_attribute = true;
  if (!m_attributes.empty()) {
    if (!ns.empty()) {
      elem->m_attributes = collect_attributes(m_node, ns, is_prefix);
    } else {
      elem->m_attributes = m_attributes;
    }
    elem->o_set("@attributes", -1, elem->m_attributes);
  }
  return elem;
}

Variant c_simplexmlelement::t_addchild(CStrRef qname,
                                       CStrRef value /* = null_string */,
                                       CStrRef ns /* = null_string */) {
  if (qname.empty()) {
    Logger::Warning("Element name is required");
    return null;
  }
  if (m_is_attribute) {
    Logger::Warning("Cannot add element to attributes");
    return null;
  }
  if (!m_node) {
    Logger::Warning("Parent is not a permanent member of the XML tree");
    return null;
  }

  xmlChar *prefix = NULL;
  xmlChar *localname = xmlSplitQName2((xmlChar *)qname.data(), &prefix);
  if (localname == NULL) {
    localname = xmlStrdup((xmlChar *)qname.data());
  }

  xmlNsPtr nsptr = NULL;
  xmlNodePtr newnode = xmlNewChild(m_node, NULL, localname,
                                   (xmlChar *)value.data());
  if (!ns.isNull()) {
    if (ns.empty()) {
      newnode->ns = NULL;
      nsptr = xmlNewNs(newnode, (xmlChar *)ns.data(), prefix);
    } else {
      nsptr = xmlSearchNsByHref(m_node->doc, m_node, (xmlChar *)ns.data());
      if (nsptr == NULL) {
        nsptr = xmlNewNs(newnode, (xmlChar *)ns.data(), prefix);
      }
      newnode->ns = nsptr;
    }
  }

  String newname((char*)localname, CopyString);
  String newns((char*)prefix, CopyString);
  xmlFree(localname);
  if (prefix) {
    xmlFree(prefix);
  }

  Array newprops = create_children(m_doc, newnode, newns, false);
  ObjectData::o_set(newprops);
  return newprops[newname];
}

void c_simplexmlelement::t_addattribute(CStrRef qname,
                                        CStrRef value /* = null_string */,
                                        CStrRef ns /* = null_string */) {
  if (qname.empty()) {
    Logger::Warning("Attribute name is required");
    return;
  }

  if (m_node && m_node->type != XML_ELEMENT_NODE) {
    m_node = m_node->parent;
  }
  if (!m_node) {
    Logger::Warning("Unable to locate parent Element");
    return;
  }

  xmlChar *prefix = NULL;
  xmlChar *localname = xmlSplitQName2((xmlChar *)qname.data(), &prefix);
  if (localname == NULL) {
    localname = xmlStrdup((xmlChar *)qname.data());
  }

  xmlAttrPtr attrp = xmlHasNsProp(m_node, localname, (xmlChar *)ns.data());
  if (attrp && attrp->type != XML_ATTRIBUTE_DECL) {
    xmlFree(localname);
    if (prefix != NULL) {
      xmlFree(prefix);
    }
    Logger::Warning("Attribute already exists");
    return;
  }

  xmlNsPtr nsptr = NULL;
  if (!ns.isNull()) {
    nsptr = xmlSearchNsByHref(m_node->doc, m_node, (xmlChar *)ns.data());
    if (nsptr == NULL) {
      nsptr = xmlNewNs(m_node, (xmlChar *)ns.data(), prefix);
    }
  }

  attrp = xmlNewNsProp(m_node, nsptr, localname, (xmlChar *)value.data());
  m_attributes.set(String((char*)localname, CopyString), value);

  xmlFree(localname);
  if (prefix != NULL) {
    xmlFree(prefix);
  }
}

String c_simplexmlelement::t___tostring() {
  Variant prop;
  if (o_properties) {
    ArrayIter iter(*o_properties);
    if (iter) {
      prop = iter.second();
    }
  }
  return prop.toString();
}

Array c_simplexmlelement::o_toIterArray(const char *context) {
  if (m_is_attribute) {
    return m_attributes;
  }
  if (!m_is_children) {
    return CREATE_VECTOR1(Object(const_cast<c_simplexmlelement*>(this)));
  }

  if (o_properties) {
    Array objs;
    for (ArrayIter iter(*o_properties); iter; ++iter) {
      if (iter.second().isObject()) {
        objs.append(iter.second());
      } else {
        for (ArrayIter iter2(iter.second()); iter2; ++iter2) {
          if (iter2.second().isObject()) {
            objs.append(iter2.second());
          }
        }
      }
    }
    return objs;
  }

  return Array();
}

Variant c_simplexmlelement::t___destruct() {
  return null;
}

///////////////////////////////////////////////////////////////////////////////
// implementing ArrayAccess

bool c_simplexmlelement::t_offsetexists(CVarRef index) {
  if (index.isInteger() && index.toInt64() == 0) {
    if (o_properties) {
      return o_properties->exists(index);
    }
    return false;
  }
  return m_attributes.exists(index);
}

Variant c_simplexmlelement::t_offsetget(CVarRef index) {
  if (index.isInteger() && index.toInt64() == 0) {
    if (o_properties) {
      return o_properties->rvalAt(index);
    }
    return null;
  }
  return m_attributes.rvalAt(index);
}

void c_simplexmlelement::t_offsetset(CVarRef index, CVarRef newvalue) {
  if (index.isInteger() && index.toInt64() == 0) {
    o_set(index.toString().data(), -1, newvalue);
    return;
  }
  m_attributes.set(index, newvalue);
}

void c_simplexmlelement::t_offsetunset(CVarRef index) {
  if (index.isInteger() && index.toInt64() == 0) {
    if (o_properties) {
      o_properties->remove(index);
    }
    return;
  }
  m_attributes.remove(index);
}

///////////////////////////////////////////////////////////////////////////////
// LibXMLError

c_libxmlerror::c_libxmlerror() {
}
c_libxmlerror::~c_libxmlerror() {
}
void c_libxmlerror::t___construct() {
}

Variant c_libxmlerror::t___destruct() {
  return null;
}

///////////////////////////////////////////////////////////////////////////////
// libxml

static ThreadLocal<std::vector<xmlError> > s_libxml_errors;

static void libxml_error_handler(void *userData, xmlErrorPtr error) {
  std::vector<xmlError> *error_list = s_libxml_errors.get();

  error_list->resize(error_list->size() + 1);
  xmlError &error_copy = error_list->back();
  memset(&error_copy, 0, sizeof(xmlError));

  if (error) {
    xmlCopyError(error, &error_copy);
  } else {
    error_copy.code = XML_ERR_INTERNAL_ERROR;
    error_copy.level = XML_ERR_ERROR;
  }
}

static Object create_libxmlerror(xmlError &error) {
  Object ret(NEW(c_libxmlerror)());
  ret->o_set("level",   -1, error.level);
  ret->o_set("code",    -1, error.code);
  ret->o_set("column",  -1, error.int2);
  ret->o_set("message", -1, String(error.message, CopyString));
  ret->o_set("file",    -1, String(error.file, CopyString));
  ret->o_set("line",    -1, error.line);
  return ret;
}

Variant f_libxml_get_errors() {
  std::vector<xmlError> *error_list = s_libxml_errors.get();
  Array ret = Array::Create();
  for (unsigned int i = 0; i < error_list->size(); i++) {
    ret.append(create_libxmlerror(error_list->at(i)));
  }
  return ret;
}

Variant f_libxml_get_last_error() {
  xmlErrorPtr error = xmlGetLastError();
  if (error) {
    return create_libxmlerror(*error);
  }
  return false;
}

void f_libxml_clear_errors() {
  xmlResetLastError();
  s_libxml_errors->clear();
}

bool f_libxml_use_internal_errors(CVarRef use_errors /* = null_variant */) {
  bool ret = (xmlStructuredError == libxml_error_handler);
  if (!use_errors.isNull()) {
    if (!use_errors.toBoolean()) {
      xmlSetStructuredErrorFunc(NULL, NULL);
      s_libxml_errors->clear();
    } else {
      xmlSetStructuredErrorFunc(NULL, libxml_error_handler);
    }
  }
  return ret;
}

void f_libxml_set_streams_context(CObjRef streams_context) {
  throw NotImplementedException(__func__);
}

///////////////////////////////////////////////////////////////////////////////
}
