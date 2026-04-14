// #pragma once

// #include <llvm/ADT/StringExtras.h>
// #include <llvm/ADT/StringRef.h>
// #include <llvm/Support/ErrorOr.h>
// #include <llvm/Support/MemoryBuffer.h>
// #include <llvm/Support/VirtualFileSystem.h>

// using source_index_t = std::size_t;

// struct src_buffer_t {
//   using bufptr_t = std::unique_ptr<llvm::MemoryBuffer>;
//   bufptr_t buffer_;

//   std::size_t length() const { return buffer_->getBufferSize(); }
//   src_buffer_t(bufptr_t ptr) : buffer_(std::move(ptr)) {}

//   static auto make(llvm::vfs::FileSystem *const fs, const std::string &filepath)
//       -> llvm::ErrorOr<src_buffer_t> {
//     auto buf = fs->getBufferForFile(filepath);
//     if (!buf)
//       return buf.getError();
//     return std::move(buf.get());
//   }

//   auto ref(const source_index_t index, const std::size_t length)
//       -> const llvm::StringRef {
//     return llvm::StringRef(buffer_->getBufferStart() + index, length);
//   }

//   auto buffer() const { return buffer_.get()->getBuffer(); }
// };

#pragma once

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

using source_index_t = std::size_t;

struct src_buffer_t {
  std::string buffer_;

  // Constructor takes ownership of the file content.
  explicit src_buffer_t(std::string&& data) : buffer_(std::move(data)) {}

  // Returns the size of the buffer.
  std::size_t length() const { return buffer_.size(); }

  // Returns a string_view for a portion of the buffer.
  std::string_view ref(const source_index_t index,
                       const std::size_t len) const {
    if (index > buffer_.size())
      throw std::out_of_range("Index is out of range");
    // Ensure the requested length does not exceed the remaining buffer.
    auto available = buffer_.size() - index;
    return std::string_view(buffer_.data() + index, std::min(len, available));
  }

  // Returns the underlying buffer.
  const std::string& buffer() const { return buffer_; }

  // Factory method to create a src_buffer_t from a file.
  // Throws std::runtime_error if the file cannot be read.
  static src_buffer_t make(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file)
      throw std::runtime_error("Failed to open file: " + filepath);
    std::ostringstream oss;
    oss << file.rdbuf();
    return src_buffer_t(oss.str());
  }
};
