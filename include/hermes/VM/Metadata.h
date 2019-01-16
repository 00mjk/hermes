#ifndef HERMES_VM_METADATA_H
#define HERMES_VM_METADATA_H

#include "hermes/ADT/OwningArray.h"
#include "hermes/Support/OptValue.h"
#include "hermes/VM/GCPointer.h"
#include "hermes/VM/HermesValue.h"
#include "hermes/VM/SymbolID.h"

#include <cassert>
#include <cstdint>
#include <map>
#include <utility>

namespace hermes {
namespace vm {

/// Metadata is a the information about a class's pointers locations.
/// This is used by the Garbage collector to know where potential pointers
/// to other objects are.
struct Metadata final {
  using offset_t = std::uint16_t;
  using OffsetAndNameAndSize =
      std::pair<offset_t, std::pair<const char *, size_t>>;
  /// Fields is a group of both offsets and names that describe a field within
  /// an object.
  struct Fields {
    Fields() = default;
    explicit Fields(size_t numFields)
        : offsets(numFields), names(numFields), sizes(numFields) {}

    size_t size() const {
      return offsets.size();
    }

    bool empty() const {
      return offsets.empty();
    }

    // Invariant: these arrays are all of the same size N, and the ith elements
    // describe the ith field.

    /// The offset location of the field within the object.
    OwningArray<offset_t> offsets;
    /// The names of the fields, only used in snapshots.
    OwningArray<const char *> names;
    /// The size, in bytes, of each field within an object.
    OwningArray<size_t> sizes;
  };

  /// The information about an array for an object.
  struct ArrayData {
    /// Which type of element the array holds.
    enum class ArrayType { Pointer, HermesValue, Symbol };
    ArrayType type;
    /// The offset of where the array starts.
    offset_t startOffset;
    /// The offset of the field that knows how many elements are in the array.
    offset_t lengthOffset;
    /// The width of each element. For example, a pointer has a stride of
    /// sizeof(void *).
    std::uint16_t stride;

    explicit ArrayData() = default;
    explicit constexpr ArrayData(
        ArrayType type,
        offset_t startOffset,
        offset_t lengthOffset,
        std::uint16_t stride)
        : type(type),
          startOffset(startOffset),
          lengthOffset(lengthOffset),
          stride(stride) {}
    ArrayData &operator=(const ArrayData &data) = default;
  };

  /// A Builder is a way to build a metadata structure by incrementally
  /// adding fields to it.
  /// After adding the fields, call \c build in order to create the \c Metadata
  /// object.
  class Builder final {
   public:
    /// Creates a Builder for an object, from a base to be used to
    /// calculate the offsets of fields from the base.
    Builder(const void *base);

    /// @name Field adders
    /// @{
    /// Adds a field to the metadata.
    /// The version without a \p name parameter means that field will not appear
    /// in snapshots.

    /// Adds a pointer field.
    void addField(const GCPointerBase *fieldLocation);
    void addField(const char *name, const GCPointerBase *fieldLocation);
    /// Adds a \c HermesValue field.
    void addField(const GCHermesValue *fieldLocation);
    void addField(const char *name, const GCHermesValue *fieldLocation);
    /// Adds a \c Symbol field.
    void addField(const SymbolID *fieldLocation);
    void addField(const char *name, const SymbolID *fieldLocation);
    /// Adds a field which has no meaning to the GC, like an integer or non-GC
    /// pointer.
    template <typename T>
    inline void addNonPointerField(const T *field);
    template <typename T>
    inline void addNonPointerField(const char *name, const T *field);

    /// @}

    /// Adds an array to this class's metadata.
    /// \p startLocation The location of the first element in the array.
    /// \p lengthLocation The location of the size of the array.
    /// The \c ArrayType parameter denotes which type of elements are in the
    /// array.
    template <ArrayData::ArrayType type, typename SizeType>
    inline void addArray(
        const void *startLocation,
        const SizeType *lengthLocation,
        std::size_t stride);

    template <ArrayData::ArrayType type, typename SizeType>
    inline void addArray(
        const char *name,
        const void *startLocation,
        const SizeType *lengthLocation,
        std::size_t stride);

    /// Build creates a Metadata, and destroys this builder.
    Metadata build();

   private:
    /// The base of the object, used to calculate offsets.
    const char *base_;
    /// A list of offsets and within the object to its field type and name.
    std::map<offset_t, std::pair<const char *, size_t>> pointers_;
    std::map<offset_t, std::pair<const char *, size_t>> values_;
    std::map<offset_t, std::pair<const char *, size_t>> symbols_;
    std::map<offset_t, std::pair<const char *, size_t>> nonPointerFields_;
    /// An optional array for an object to contain.
    OptValue<ArrayData> array_;

    friend Metadata;
  };

  /// Construct an empty metadata.
  Metadata() = default;
  Metadata(Metadata &&);
  /// Construct from a builder.
  Metadata(Builder &&mb);
  ~Metadata() = default;

  Metadata &operator=(Metadata &&) = default;

  /// A mapping from an offset to a name for that field
  Fields pointers_;
  Fields values_;
  Fields symbols_;
  /// Fields that are not pointers that have special meaning to the GC.
  Fields nonPointerFields_;

  /// The optional array for this object to hold.
  /// NOTE: this format currently does not support multiple arrays.
  OptValue<ArrayData> array_;
};

/// @name Formatters
/// @{

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const Metadata &meta);
llvm::raw_ostream &operator<<(llvm::raw_ostream &os, Metadata::ArrayData array);
llvm::raw_ostream &operator<<(
    llvm::raw_ostream &os,
    Metadata::ArrayData::ArrayType arraytype);

/// @}

/// @name Inline implementations
/// @{

template <typename T>
inline void Metadata::Builder::addNonPointerField(const T *field) {
  addNonPointerField(nullptr, field);
}

template <typename T>
inline void Metadata::Builder::addNonPointerField(
    const char *name,
    const T *field) {
  // Metadata currently only allows unsigned 32 and 64 bit integers.
  // This is because for each size the visitor that uses this metadata needs
  // a unique size in order to determine how to interpret a type.
  // In order to expand this, there would need to be a concept of unique type
  // tags.
  static_assert(
      sizeof(T) == sizeof(std::uint32_t) || sizeof(T) == sizeof(std::uint64_t),
      "field must be either 4 or 8 bytes");
  const auto key = reinterpret_cast<const char *>(field) - base_;
  nonPointerFields_[key] = std::make_pair(name, sizeof(T));
}

template <Metadata::ArrayData::ArrayType type, typename SizeType>
inline void Metadata::Builder::addArray(
    const void *startLocation,
    const SizeType *lengthLocation,
    std::size_t stride) {
  addArray<type, SizeType>(nullptr, startLocation, lengthLocation, stride);
}

template <Metadata::ArrayData::ArrayType type, typename SizeType>
inline void Metadata::Builder::addArray(
    const char *name,
    const void *startLocation,
    const SizeType *lengthLocation,
    std::size_t stride) {
  static_assert(
      (sizeof(SizeType) == sizeof(std::int32_t)),
      "The size parameter of an array should only be a 32 bit value");
  array_ = ArrayData(
      type,
      reinterpret_cast<const char *>(startLocation) - base_,
      reinterpret_cast<const char *>(lengthLocation) - base_,
      stride);
}

/// @}

} // namespace vm
} // namespace hermes

#endif // HERMES_VM_METADATA_H
