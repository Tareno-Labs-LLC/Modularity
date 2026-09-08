#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace ANIX {

class Plan {
public:
    void Set(const std::string& key, const std::string& value) {
        operations_.push_back("{\"op\":\"set\",\"key\":" + Quote(key) + ",\"value\":" + Quote(value) + "}");
    }

    void Enable(const std::string& feature) {
        operations_.push_back("{\"op\":\"enable\",\"feature\":" + Quote(feature) + "}");
    }

    void Disable(const std::string& feature) {
        operations_.push_back("{\"op\":\"disable\",\"feature\":" + Quote(feature) + "}");
    }

    void Package(const std::string& name) {
        operations_.push_back("{\"op\":\"package.add\",\"name\":" + Quote(name) + "}");
    }

    void RemovePackage(const std::string& name) {
        operations_.push_back("{\"op\":\"package.remove\",\"name\":" + Quote(name) + "}");
    }

    std::string Json() const {
        std::ostringstream out;
        out << "{\"planVersion\":1,\"language\":\"moducpp\",\"operations\":[";
        for (std::size_t i = 0; i < operations_.size(); ++i) {
            if (i != 0) out << ',';
            out << operations_[i];
        }
        out << "]}";
        return out.str();
    }

    void Finish() const { std::cout << Json() << '\n'; }

private:
    static std::string Quote(const std::string& value) {
        std::ostringstream out;
        out << '"';
        for (char c : value) {
            switch (c) {
                case '"': out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n"; break;
                case '\r': out << "\\r"; break;
                case '\t': out << "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) out << '?';
                    else out << c;
            }
        }
        out << '"';
        return out.str();
    }

    std::vector<std::string> operations_;
};

} // namespace ANIX
