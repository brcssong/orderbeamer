//
// Created by Brian Song on 5/18/26.
//

#ifndef ORDERBEAMER_ERROR_H
#define ORDERBEAMER_ERROR_H
#include <iostream>
#include <string_view>
#include "engine/statics.h"
#include "../memory/Buffer.h"

// Lightweight Error-Buffer Handler
namespace Error {
    static void generate(Buffer<ErrorRecord, memory_constants::ERROR_BUF_SIZE>& errorBuf, const std::string_view& message, const int errNum) {
        // Forget if error buffer is already full to reduce latency
        errorBuf.tryWrite(ErrorRecord {message, errNum});
    }

    static void output(const std::string_view& message, const int errNum) noexcept {
        std::cout << "ERROR ENCOUNTERED: " << message << " WITH ERROR CODE " << errNum << "\n";
    }

    static void generate(const std::string_view& message, const int errNum) noexcept {
        // If no error buffer specified, directly process in cout (when latency is not a hard requirement)
        output(message, errNum);
    }
};


#endif //ORDERBEAMER_ERROR_H
