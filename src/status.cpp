#include "lsmkv/status.h"

namespace lsmkv {

std::string Status::ToString() const {
    const char* type = nullptr;
    switch (code_) {
        case kOk: return "OK";
        case kNotFound: type = "NotFound"; break;
        case kCorruption: type = "Corruption"; break;
        case kNotSupported: type = "NotSupported"; break;
        case kInvalidArgument: type = "InvalidArgument"; break;
        case kIOError: type = "IOError"; break;
    }
    std::string result(type);
    if (!msg_.empty()) {
        result.append(": ");
        result.append(msg_);
    }
    return result;
}

}  // namespace lsmkv
