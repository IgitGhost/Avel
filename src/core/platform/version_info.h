#pragma once

#include <string>

namespace avel::platform {

// What a program file's version resource says about it.
struct FileProduct {
    std::wstring product_name;  // ProductName, or FileDescription when there is none
    std::wstring company_name;
};

// Reads the first language of the file's version resource; empty fields when it has none.
FileProduct file_product(const std::wstring& path);

}  // namespace avel::platform
