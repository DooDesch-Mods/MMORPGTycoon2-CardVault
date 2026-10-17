#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace vault {
using Json = nlohmann::json;
namespace fs = std::filesystem;
struct HttpError : std::runtime_error {
  unsigned status;
  HttpError(unsigned code,const std::string& cause):std::runtime_error(cause),status(code){}
};
inline fs::path utf8path(const std::string& value){return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));}
inline std::string utf8text(const fs::path& value){auto text=value.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
struct HttpResponse { unsigned status=0; std::vector<unsigned char> bytes; std::string sha256; Json json() const; };
HttpResponse http(const std::string& method,const std::string& path,const std::string& token={},const std::vector<unsigned char>& body={},bool png=false,const std::string& confirmation={});
std::string hash(const std::vector<unsigned char>& data);
std::string random_id();
std::vector<unsigned char> read_file(const fs::path& path);
void write_new(const fs::path& path,const std::vector<unsigned char>& data);
void validate_card(const std::vector<unsigned char>& png);
void preview_rgba(const std::vector<unsigned char>& png,const fs::path& destination);
void save_connection(const fs::path& root,const Json& value);
Json load_connection(const fs::path& root);
void open_browser(const std::string& url);
namespace game {
void initialize();
fs::path card_directory(const fs::path& profile);
std::string selected_character();
fs::path export_selected(const fs::path& profile);
void refresh_library();
}
}
