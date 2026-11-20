#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace msa::xlsx {
struct Node {
    enum Kind { Open, Text, Close } kind;
    std::string name, text;
    std::map<std::string, std::string> attributes;
    std::string get(const std::string &key) const {
        const auto it = attributes.find(key);
        return it == attributes.end() ? std::string{} : it->second;
    }
};
void read_xml(const std::string &bytes, const std::function<void(const Node &)> &visit);
class Package {
  public:
    explicit Package(const std::string &bytes);
    ~Package();
    std::string part(const std::string &name) const;
    std::string relationships(const std::string &source) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
std::string zip_stored(const std::vector<std::pair<std::string, std::string>> &parts);
} // namespace msa::xlsx
