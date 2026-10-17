#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <filesystem>
#include <stdexcept>

namespace vault {
inline std::string card_basename(const std::string& title) {
    std::string name;
    for (unsigned char c : title) {
        bool space = c <= 32 || c == 127 || std::string("<>:\"/\\|?*").find(char(c)) != std::string::npos;
        if (space) { if (!name.empty() && name.back() != ' ') name += ' '; }
        else name += char(c);
    }
    auto trim = [&] {
        while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
        auto first = name.find_first_not_of(" .");
        name = first == std::string::npos ? std::string() : name.substr(first);
    };
    trim();
    if (name.size() > 80) {
        size_t end = 80;
        while (end && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
        name.resize(end); trim();
    }
    if (name.empty()) name = "Character card";
    auto device = name.substr(0, name.find('.'));
    while (!device.empty() && device.back() == ' ') device.pop_back();
    for (auto& c : device) c = char(std::toupper(static_cast<unsigned char>(c)));
    bool reserved = device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" ||
        (device.size() == 4 && (device.starts_with("COM") || device.starts_with("LPT")) && device[3] >= '0' && device[3] <= '9');
    if (reserved) name = "Card " + name;
    return name;
}
template<class Digest> std::filesystem::path readable_card_path(const std::filesystem::path& directory,const std::string& title,const std::string& digest,Digest file_digest) {
    auto occupied = [&](const auto& path) {
        return std::filesystem::exists(path) && (std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path) || file_digest(path) != digest);
    };
    auto base=card_basename(title);
    auto path=[](const std::string& value){return std::filesystem::path(std::u8string(value.begin(),value.end()));};
    auto destination=directory/path(base+".png");
    if(occupied(destination)) destination=directory/path(base+" - "+digest.substr(0,12)+".png");
    if(occupied(destination)) throw std::runtime_error("A different card already uses the conflict filename. No card was overwritten.");
    return destination;
}
}
