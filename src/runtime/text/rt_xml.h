//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_xml.h
// Purpose: XML parsing and tree-building API for Zanna.Data.Xml. Supports
//          elements, attributes, text nodes, comments, and CDATA sections
//          with a reference-counted node tree and compact/pretty serialisation.
// Key invariants:
//   - Parses a practical XML subset (no DTD entity expansion, namespace, or
//     UTF-8 validation); returns NULL on input the subset rejects.
//   - Tree nodes are reference-counted; children are retained by their parent.
//   - rt_xml_format produces compact output; rt_xml_format_pretty adds indentation.
//   - All node handles carry RT_XML_NODE_CLASS_ID; wrong-type handles return safe defaults.
// Ownership/Lifetime:
//   - Returned node objects start with refcount 1; caller owns the root.
//   - Releasing the root releases all children transitively.
//   - rt_xml_append/rt_xml_insert retain the child; caller may release its own ref.
// Links: src/runtime/text/rt_xml.c (implementation),
//        src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Public C ABI for parsing, constructing, traversing, and serializing
///        reference-counted XML trees.
/// @details The parser accepts the runtime's practical XML subset: it builds
///          elements, text, comments, and CDATA nodes; skips processing
///          instructions and DTD declarations; decodes numeric references and
///          the five predefined entities; and requires exactly one document
///          element. Returned strings, sequences, documents, and wrapper
///          objects are owned unless a function explicitly documents a
///          borrowed node reference.
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//=========================================================================
// XML Node Types
//=========================================================================

/// @brief Runtime class identifier for XML node handles.
#define RT_XML_NODE_CLASS_ID INT64_C(-0x44584D4C)

/// @brief Kinds stored in @c xml_node objects and returned by
///        @ref rt_xml_node_type.
typedef enum {
    XML_NODE_ELEMENT = 1, ///< Element node (has tag, attributes, children)
    XML_NODE_TEXT = 2,    ///< Text content
    XML_NODE_COMMENT = 3, ///< Comment node (<!-- ... -->)
    XML_NODE_CDATA = 4,   ///< CDATA section (<![CDATA[ ... ]]>)
    XML_NODE_DOCUMENT = 5 ///< Document root
} rt_xml_node_type_t;

//=========================================================================
// XML Parsing
//=========================================================================

/// @brief Parse an XML byte string into a document tree.
/// @details The source must be nonempty and contain exactly one root element.
///          Comments and CDATA sections become nodes; processing instructions
///          and DOCTYPE declarations are consumed but not represented.
/// @param text Runtime string containing the XML source.
/// @return Owned document node, or null when the input is empty or rejected.
/// @note Call @ref rt_xml_error after failure to obtain the thread-local
///       diagnostic.
void *rt_xml_parse(rt_string text);

/// @brief Parse XML string into a Zanna.Result.
/// @details Returns `Ok(document)` for well-formed XML and `Err(message)` for
///          malformed or empty input. The returned Result retains the parsed
///          document, allowing callers to handle failures without reading the
///          thread-local rt_xml_error() side channel.
/// @param text Runtime string containing the XML source.
/// @return Owned opaque `Zanna.Result` containing the document or error string.
void *rt_xml_parse_result(rt_string text);

/// @brief Copy the current thread's most recent XML diagnostic.
/// @return Owned error-message string, or an owned empty string if none exists.
rt_string rt_xml_error(void);

/// @brief Test whether source text is accepted by the practical XML parser.
/// @details Parses and releases a temporary document. This is not full
///          validating XML processing: DTD content and namespaces are not
///          interpreted, and non-ASCII name bytes are accepted bytewise.
/// @param text Candidate XML source.
/// @return 1 when parsing succeeds, otherwise 0.
int8_t rt_xml_is_valid(rt_string text);

/// @brief Test whether an opaque runtime object is an XML node handle.
/// @param node Candidate runtime object; null is permitted.
/// @return 1 for a live XML node of the expected runtime class and size,
///         otherwise 0.
int8_t rt_xml_is_node(void *node);

//=========================================================================
// Node Creation
//=========================================================================

/// @brief Create a detached element with no attributes or children.
/// @param tag Valid nonempty XML element name; retained by the node.
/// @return Owned element node, or null after recording an invalid-name or
///         allocation error.
void *rt_xml_element(rt_string tag);

/// @brief Create a detached text node.
/// @param content Text content to retain; null creates an empty node.
/// @return Owned text node, or null when the content contains a forbidden XML
///         control character or allocation fails.
void *rt_xml_text(rt_string content);

/// @brief Create a detached XML comment node.
/// @param content Comment body without delimiters; null creates an empty body.
/// @return Owned comment node, or null if the body contains `--`, ends in `-`,
///         contains a forbidden control character, or allocation fails.
void *rt_xml_comment(rt_string content);

/// @brief Create a detached CDATA-section node.
/// @param content Raw body without delimiters; null creates an empty body.
/// @return Owned CDATA node, or null if the body contains `]]>`, a forbidden
///         control character, or allocation fails.
void *rt_xml_cdata(rt_string content);

//=========================================================================
// Node Properties
//=========================================================================

/// @brief Read an XML node's kind.
/// @param node XML node to inspect.
/// @return One of @ref rt_xml_node_type_t, or zero for an invalid handle.
int64_t rt_xml_node_type(void *node);

/// @brief Copy the tag name of an element node.
/// @param node XML node to inspect.
/// @return Owned tag string, or an owned empty string for non-elements and
///         invalid handles.
rt_string rt_xml_tag(void *node);

/// @brief Copy a node's raw content slot.
/// @details Text, comment, and CDATA nodes populate this slot; elements and
///          documents do not.
/// @param node XML node to inspect.
/// @return Owned content string, or an owned empty string when unavailable.
rt_string rt_xml_content(void *node);

/// @brief Concatenate text and CDATA content in document order.
/// @details Element and document subtrees are traversed iteratively; comments
///          do not contribute text.
/// @param node XML node whose textual value is requested.
/// @return Owned combined string, empty for invalid or nontextual nodes.
rt_string rt_xml_text_content(void *node);

//=========================================================================
// Attributes
//=========================================================================

/// @brief Copy an element attribute's value.
/// @param node Element node to query.
/// @param name Case-sensitive attribute name.
/// @return Owned value string, or an owned empty string when absent or invalid.
/// @note Use @ref rt_xml_has_attr to distinguish an absent attribute from one
///       whose value is empty.
rt_string rt_xml_attr(void *node, rt_string name);

/// @brief Test whether an element has a named attribute.
/// @param node Element node to query.
/// @param name Case-sensitive attribute name.
/// @return 1 when present, otherwise 0.
int8_t rt_xml_has_attr(void *node, rt_string name);

/// @brief Add or replace an element attribute.
/// @details A null value is stored as an empty string. Invalid names and
///          forbidden value characters record an XML error; non-elements are
///          unchanged.
/// @param node Element node to mutate.
/// @param name Valid XML attribute name.
/// @param value Attribute value, or null for an empty value.
void rt_xml_set_attr(void *node, rt_string name, rt_string value);

/// @brief Remove a named element attribute.
/// @param node Element node to mutate.
/// @param name Case-sensitive attribute name.
/// @return 1 if removed, or 0 if absent or invalid.
int8_t rt_xml_remove_attr(void *node, rt_string name);

/// @brief Snapshot all attribute names from an element.
/// @param node Element node to inspect.
/// @return Owned sequence of owned name strings, empty for invalid or
///         attribute-less nodes.
void *rt_xml_attr_names(void *node);

//=========================================================================
// Children
//=========================================================================

/// @brief Snapshot a node's direct children.
/// @param node Element or document node to inspect.
/// @return Owned sequence retaining its child nodes, empty for invalid or
///         childless inputs, or null if the sequence cannot be allocated.
void *rt_xml_children(void *node);

/// @brief Count a node's direct children.
/// @param node Element or document node to inspect.
/// @return Number of direct children, or zero for invalid or childless nodes.
int64_t rt_xml_child_count(void *node);

/// @brief Borrow the child at a zero-based index.
/// @param node Element or document node to inspect.
/// @param index Zero-based child index.
/// @return Borrowed child node, or null if the index or handle is invalid.
void *rt_xml_child_at(void *node, int64_t index);

/// @brief Find the first direct child element having a tag.
/// @param node Element or document node whose children are searched.
/// @param tag Case-sensitive tag name.
/// @return Borrowed first matching child, or null when absent.
void *rt_xml_child(void *node, rt_string tag);

/// @brief Collect all direct child elements having a tag.
/// @param node Element or document node whose children are searched.
/// @param tag Case-sensitive tag name.
/// @return Owned sequence retaining matching elements in child order.
void *rt_xml_children_by_tag(void *node, rt_string tag);

/// @brief Append a detached node to an element or document.
/// @details Document children, cycles, and children already owned by another
///          parent are rejected. Re-appending the same child to its existing
///          parent is a no-op.
/// @param node Parent element or document node.
/// @param child Detached non-document node to append.
/// @note The parent retains @p child; callers may release their own reference.
void rt_xml_append(void *node, void *child);

/// @brief Insert a detached child at a zero-based index.
/// @details Indices beyond the end append; negative indices do nothing.
///          Document children, cycles, and children owned by another parent are
///          rejected.
/// @param node Parent element or document node.
/// @param index Zero-based insertion position.
/// @param child Detached non-document node to insert.
/// @note The parent retains @p child; callers may release their own reference.
void rt_xml_insert(void *node, int64_t index, void *child);

/// @brief Detach a direct child by object identity.
/// @param node Parent element or document node.
/// @param child Direct child to remove.
/// @return 1 after removal, or 0 when not found or invalid.
int8_t rt_xml_remove(void *node, void *child);

/// @brief Detach the child at an index.
/// @param node Parent element or document node.
/// @param index Zero-based child index; invalid indices are ignored.
void rt_xml_remove_at(void *node, int64_t index);

/// @brief Replace an element's children with at most one text node.
/// @details Null or empty text leaves the element childless. Invalid XML
///          control characters preserve the original children and record an
///          error.
/// @param node Element node to mutate.
/// @param text New text content.
void rt_xml_set_text(void *node, rt_string text);

//=========================================================================
// Navigation
//=========================================================================

/// @brief Obtain a retained reference to a node's parent.
/// @param node XML node to inspect.
/// @return Retained parent node, or null for a root, detached, or invalid node.
void *rt_xml_parent(void *node);

/// @brief Borrow the document element at the top of a node's tree.
/// @details The function climbs from descendants to the topmost ancestor and
///          returns that element directly for detached element trees.
/// @param doc Document node or any descendant node.
/// @return Borrowed root element, or null if no element exists.
void *rt_xml_root(void *doc);

/// @brief Find elements by tag name or slash-separated path.
/// @details Plain tags use pre-order traversal and include @p node itself. A
///          slash in @p tag selects the runtime's simple segment-by-segment
///          path search.
/// @param node Root of the subtree to search.
/// @param tag Case-sensitive tag or slash-separated element path.
/// @return Owned sequence retaining all matches in document order.
void *rt_xml_find_all(void *node, rt_string tag);

/// @brief Find first element by tag name or slash-separated path.
/// @param node Root of the subtree to search.
/// @param tag Case-sensitive tag or slash-separated element path.
/// @return Retained first matching element, or null if absent or invalid.
void *rt_xml_find(void *node, rt_string tag);

/// @brief Find first element by tag name or path as an Option.
/// @details Returns `Some(node)` when a match exists and `None` when absent,
///          avoiding the legacy NULL sentinel.
/// @param node Root of the subtree to search.
/// @param tag Case-sensitive tag or slash-separated element path.
/// @return Owned opaque `Zanna.Option` containing the first match, or None.
void *rt_xml_find_option(void *node, rt_string tag);

//=========================================================================
// Formatting
//=========================================================================

/// @brief Serialize a node without adding formatting whitespace.
/// @details Text and attribute content is escaped, empty elements self-close,
///          and no XML declaration is emitted.
/// @param node XML node to serialize.
/// @return Owned XML string, or an owned empty string for an invalid node.
rt_string rt_xml_format(void *node);

/// @brief Serialize a node using indentation for block-only content.
/// @details The indentation width is clamped to 0..8 spaces. Text-only and
///          mixed-content elements remain inline to avoid semantic changes;
///          any final formatting newline is removed.
/// @param node XML node to serialize.
/// @param indent Requested spaces per nesting level.
/// @return Owned serialized string, empty for an invalid node.
rt_string rt_xml_format_pretty(void *node, int64_t indent);

//=========================================================================
// Utility
//=========================================================================

/// @brief Escape a string for safe use in XML text or a quoted attribute.
/// @details Replaces `&`, `<`, `>`, double quote, and apostrophe with named
///          entities. Forbidden C0 controls become `&#xFFFD;`.
/// @param text Text to escape; null is treated as empty.
/// @return Owned escaped string.
rt_string rt_xml_escape(rt_string text);

/// @brief Decode numeric references and the five predefined XML entities.
/// @details Unknown, malformed, and forbidden numeric references remain
///          unchanged in the result.
/// @param text Entity-containing text; null is treated as empty.
/// @return Owned unescaped string.
rt_string rt_xml_unescape(rt_string text);

#ifdef __cplusplus
}
#endif
