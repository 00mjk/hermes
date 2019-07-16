/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#ifndef HERMES_VM_HEAPSNAPSHOT_H
#define HERMES_VM_HEAPSNAPSHOT_H

#include "hermes/Public/GCConfig.h"
#include "hermes/Support/JSONEmitter.h"
#include "hermes/VM/CellKind.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/StringRefUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <bitset>
#include <string>

namespace hermes {
namespace vm {

using HeapSizeType = uint32_t;
class StringPrimitive;

/// Escapes the control characters in a UTF-8 string. Used for embedding an
/// arbitrary string into JSON.
std::string escapeJSON(llvm::StringRef s);
/// @name Converters from arbitrary types to string
/// @{
std::string converter(const char *name);
std::string converter(unsigned index);
std::string converter(int index);
std::string converter(const StringPrimitive *str);
std::string converter(UTF16Ref ref);
/// @}

/// A serialization formatter for a heap snapshot that outputs a
/// Facebook-specific data format. This format is JSON, and the full format is
/// documented at TODO insert link here.
class FacebookHeapSnapshot {
 public:
  static constexpr auto version = 5;
  using ObjectID = uintptr_t;
  struct Object {
    /// The unique identifier for the object.
    const ObjectID id;
    /// The type of the object.
    const CellKind type;
    /// The size of the object at runtime.
    const HeapSizeType size;

    explicit Object() : id(0), type(CellKind::UninitializedKind), size(0) {}
    explicit Object(ObjectID id, CellKind type, HeapSizeType size)
        : id(id), type(type), size(size) {}
  };

  /// Creates a new empty heap snapshot. Add objects to it with \see addObject.
  /// \p os The stream to write the output to as JSON.
  /// \p compact Whether the JSON should be compact or pretty.
  /// NOTE: this constructor writes to \p os.
  explicit FacebookHeapSnapshot(
      llvm::raw_ostream &os,
      bool compact,
      gcheapsize_t totalHeapSize);

  /// NOTE: this destructor writes to \p os.
  ~FacebookHeapSnapshot();

  /// Markers for the beginning of sections.
  void beginRoots();
  void endRoots();
  void beginRefs();
  void endRefs();

  /// Adds an object, but specifies it as a root of the graph.
  void addRoot(ObjectID id);
  /// Starts a new object in the stream.
  /// Must be called after any other `startObject` has been ended.
  void startObject(Object &&o);
  /// Starts a new object which also has a displayable value.
  /// \p value The value to use for the object being started.
  void startObjectWithValue(Object &&o, const StringPrimitive *value) {
    const static std::string quote("\"");
    std::string convertedValue(quote + escapeJSON(converter(value)) + quote);
    startObject(std::move(o), convertedValue.c_str());
  }
  /// Add a name-to-pointer association to the current object being tracked
  void addToCurrentObject(const char *name, ObjectID objId);
  /// Add a name-to-value association to the current object being tracked
  void addValueToCurrentObject(const char *name, double value);
  void addValueToCurrentObject(const char *name, bool value);
  /// Add a symbolID to the current object being tracked as a 32-bit integer
  void addSymbolIdToCurrentObject(const char *name, uint32_t value);
  /// Add an internal value to the current object being tracked
  void addInternalToCurrentObject(const char *name, uint64_t value);
  /// Add a property with a null value to the current object being tracked
  void addNullToCurrentObject(const char *name);
  /// Add a property with an undefined value to the current object being tracked
  void addUndefinedToCurrentObject(const char *name);
  /// Add native value property to the current object being tracked
  void addNativeValueToCurrentObject(const char *name, uint64_t value);
  /// Add empty value property to the current object being tracked
  void addEmptyToCurrentObject(const char *name);
  /// Helper function that extracts hermes value to add it to the snapshot
  void addHermesValueToCurrentObject(const char *name, HermesValue &hv);
  void endObject();
  /// Write the beggining of the Identifier table format
  void beginIdTable();
  /// Writes an entry of the identifier table
  void addIdTableEntry(UTF16Ref entry, uint32_t id);
  /// Writes the end of the Identifier table format
  void endIdTable();

 private:
  llvm::raw_ostream &os_;
  /// If no object has been added to the stream yet, don't prepend a comma.
  bool firstObj_{true};
  /// If no edge has been added to the current object yet.
  bool firstProperty_{true};
  /// If no entries have been added to the Identigier Table array_
  bool firstIdTableEntry_{true};
  /// Whether the JSON should be compact or pretty.
  bool compact_;

  /// Helper function after conversion of the ValueType.
  /// \p value If non-null, the string value to be output into the JSON stream,
  ///   else the object has no value.
  void startObject(Object &&o, const char *value);

  /// Helper Function that writes the name of the property, which is written
  /// the same way for any value type
  void startProperty(const char *name);
};

/// A raw dump of the memory as a binary stream.
///
/// It outputs the entire heap, byte for byte, to the given \p os, but first
/// outputs the \p start address to be used as a base for all pointers upon
/// reconstruction.
/// TODO: This format currently does not preserve perfect information, for
///   example NativeFunctionPtr will be randomized on each startup, and cannot
///   be guaranteed to point at the same place.
///   Currently it also does not do anything about string pointers, so those
///   will also be invalid.
///   Something which enables these should be implemented in the future.
void rawHeapSnapshot(llvm::raw_ostream &os, const char *start, const char *end);

class V8HeapSnapshot {
 public:
  using StringID = uint32_t;
  struct Node {
    /// The highest-level categorization of the type of an object.
    /// NOTE: These types are chosen to align with v8's types, not what Hermes
    /// actually uses.
    enum class Type {
      Hidden,
      Array,
      String,
      Object,
      Code,
      Closure,
      Regexp,
      Number,
      Native,
      // Synthetic means it's not shown to the user, but only exists to meet the
      // requirements of a graph (for example, the GC roots are synthetic).
      Synthetic,
      ConcatenatedString,
      SlicedString,
      Symbol,
      BigInt,
      NumTypes,
    };

    using ID = uintptr_t;
    using Index = uint32_t;

    Type type;
    StringID name;
    ID id;
    HeapSizeType selfSize;
    HeapSizeType edgeCount;
    HeapSizeType traceNodeId;

    explicit Node(
        Type type,
        StringID name,
        ID id,
        HeapSizeType selfSize,
        HeapSizeType edgeCount,
        HeapSizeType traceNodeId = 0)
        : type(type),
          name(name),
          id(id),
          selfSize(selfSize),
          edgeCount(edgeCount),
          traceNodeId(traceNodeId) {}

    static const char *nodeTypeStr(Type type);
    static Type cellKindToType(CellKind kind);
  };

  struct Edge {
    enum class Type {
      // NOTE: Keep this in sync with the list emitted in endMeta.
      Context,
      Element,
      Property,
      Internal,
      Hidden,
      Shortcut,
      Weak
    };
    using Index = uint32_t;
    // TODO: possibly kill PointerKind in favour of switching on type directly
    enum PointerKind { PKNamed, PKUnnamed };
    struct Named {};
    struct Unnamed {};
    Type type;
    PointerKind pointerKind;
    Node::ID toNode;

    StringID name() const {
      assert(pointerKind == PKNamed);
      return name_;
    }

    Index index() const {
      assert(pointerKind == PKUnnamed);
      return index_;
    }

    explicit Edge(Named tag, Type type, Node::ID toNode, StringID name)
        : type(type), pointerKind(PKNamed), toNode(toNode), name_(name) {}

    explicit Edge(Unnamed tag, Type type, Node::ID toNode, Index index)
        : type(type), pointerKind(PKUnnamed), toNode(toNode), index_(index) {}

   private:
    StringID name_; // valid only if pointerKind == PKNamed
    Index index_; // valid only if pointerKind == PKUnnamed
  };

  explicit V8HeapSnapshot(JSONEmitter &json);

  /// NOTE: this destructor writes to \p json.
  ~V8HeapSnapshot();

  void beginNodes();
  void addNode(Node &&node);
  void endNodes();

  void beginEdges();
  void addEdge(Edge &&edge);
  void endEdges();

  void emitMeta();

  void beginTraceFunctionInfos();
  void endTraceFunctionInfos();

  void beginTraceTree();
  void endTraceTree();

  void beginSamples();
  void endSamples();

  void beginLocations();
  void endLocations();

  void beginStrings();
  void addString(llvm::StringRef str);
  void endStrings();

 private:
  enum class Section {
    Nodes,
    Edges,
    TraceFunctionInfos,
    TraceTree,
    Samples,
    Locations,
    Strings
  };

  JSONEmitter &json_;
  std::bitset<8> sectionsWritten_;
  llvm::Optional<Section> currentSection_;
  llvm::DenseMap<Node::ID, Node::Index> nodeToIndex_;
  Node::Index nodeCount_{0};
#ifndef NDEBUG
  /// How many edges have currently been added.
  Edge::Index edgeCount_{0};
  /// How many edges there are expected to be. Used for checking the correctness
  /// of the snapshot.
  Edge::Index expectedEdges_{0};
#endif

  void beginSection(Section section);
  void endSection(Section section);
  bool didWriteSection(Section section) const;

  static size_t sectionIndex(Section section);
  static uint32_t edgeTypeIndex(Edge::Type);
  static uint32_t nodeTypeIndex(Node::Type);
};

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_HEAPSNAPSHOT_H
