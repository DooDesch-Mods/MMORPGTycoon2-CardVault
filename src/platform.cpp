#include "vault.h"
#include <winhttp.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <array>
#include <iomanip>
#include <sstream>
#include <cstring>

namespace vault {
using Microsoft::WRL::ComPtr;
namespace {
constexpr size_t limit = 1024*1024;
void require(bool okay,const std::string& message){if(!okay)throw std::runtime_error(message);}
std::wstring wide(const std::string& text){int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);require(count>0,"Invalid UTF-8 text");std::wstring result(count,0);MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),result.data(),count);return result;}
struct Internet {HINTERNET value=nullptr;~Internet(){if(value)WinHttpCloseHandle(value);}operator HINTERNET()const{return value;}};
void checked(BOOL result,const char* operation){require(result!=FALSE,std::string(operation)+" failed (Windows "+std::to_string(GetLastError())+")");}
std::vector<unsigned char> protected_bytes(const std::vector<unsigned char>& input,bool encrypt){
  DATA_BLOB in{DWORD(input.size()),const_cast<BYTE*>(input.data())},out{};
  BOOL okay=encrypt?CryptProtectData(&in,L"MT2 Card Vault",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out):CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out);
  checked(okay,encrypt?"Save connection":"Read connection");std::vector<unsigned char> result(out.pbData,out.pbData+out.cbData);LocalFree(out.pbData);return result;
}
}
Json HttpResponse::json()const{return Json::parse(bytes.begin(),bytes.end());}
std::string hash(const std::vector<unsigned char>& data){
  BCRYPT_ALG_HANDLE algorithm=nullptr;require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"SHA256 provider unavailable");
  std::array<unsigned char,32> digest{};auto status=BCryptHash(algorithm,nullptr,0,const_cast<PUCHAR>(data.data()),ULONG(data.size()),digest.data(),ULONG(digest.size()));BCryptCloseAlgorithmProvider(algorithm,0);require(status>=0,"SHA256 failed");
  std::ostringstream text;for(auto byte:digest)text<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(byte);return text.str();
}
std::string random_id(){std::vector<unsigned char> value(16);require(BCryptGenRandom(nullptr,value.data(),ULONG(value.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0,"Random generator failed");return hash(value).substr(0,24);}
HttpResponse http(const std::string& method,const std::string& path,const std::string& token,const std::vector<unsigned char>& body,bool png,const std::string& confirmation){
  require(path.starts_with("/api/mod/v1/")&&path.find_first_of("\r\n") == std::string::npos,"Invalid Card Vault API path");
  auto origin=wide(CARDVAULT_ORIGIN);URL_COMPONENTS parts{sizeof(parts)};parts.dwHostNameLength=DWORD(-1);parts.dwUrlPathLength=DWORD(-1);parts.dwUserNameLength=DWORD(-1);parts.dwPasswordLength=DWORD(-1);
  checked(WinHttpCrackUrl(origin.c_str(),DWORD(origin.size()),0,&parts),"Read service URL");
  require(parts.nScheme==INTERNET_SCHEME_HTTPS&&parts.dwUserNameLength==0&&parts.dwPasswordLength==0,"Card Vault requires HTTPS without URL credentials");
  std::wstring hostname(parts.lpszHostName,parts.dwHostNameLength);
  Internet session{WinHttpOpen(L"MT2CardVault/0.1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0)};require(session.value,"HTTP session failed");
  checked(WinHttpSetTimeouts(session,5000,5000,10000,15000),"Set network timeout");
  Internet connection{WinHttpConnect(session,hostname.c_str(),parts.nPort,0)};require(connection.value,"HTTP connection failed");
  auto verb=wide(method),url=wide(path);Internet request{WinHttpOpenRequest(connection,verb.c_str(),url.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};require(request.value,"HTTP request failed");
  DWORD redirects=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;checked(WinHttpSetOption(request,WINHTTP_OPTION_REDIRECT_POLICY,&redirects,sizeof(redirects)),"Disable redirects");
  std::string headers="Accept: application/json\r\n";
  if(!token.empty()){require(token.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")==std::string::npos,"Invalid connection token");headers+="Authorization: Bearer "+token+"\r\n";}
  if(method=="POST")headers+=png?"Content-Type: image/png\r\n":"Content-Type: application/json\r\n";
  if(!confirmation.empty()){require(confirmation.size()==64&&confirmation.find_first_not_of("0123456789abcdef")==std::string::npos,"Invalid upload confirmation");headers+="X-Confirm-Upload: "+confirmation+"\r\n";}
  auto header=wide(headers);require(body.size()<=limit,"Request exceeds 1 MiB");
  checked(WinHttpSendRequest(request,header.c_str(),DWORD(header.size()),body.empty()?WINHTTP_NO_REQUEST_DATA:const_cast<unsigned char*>(body.data()),DWORD(body.size()),DWORD(body.size()),0),"Send Card Vault request");
  checked(WinHttpReceiveResponse(request,nullptr),"Receive Card Vault response");
  HttpResponse response;DWORD size=sizeof(response.status);checked(WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&response.status,&size,WINHTTP_NO_HEADER_INDEX),"Read HTTP status");
  wchar_t digest[128]{};size=sizeof(digest);if(WinHttpQueryHeaders(request,WINHTTP_QUERY_CUSTOM,L"X-Card-SHA256",digest,&size,WINHTTP_NO_HEADER_INDEX)){for(auto c:std::wstring(digest))response.sha256+=char(c);}
  for(;;){DWORD available=0;checked(WinHttpQueryDataAvailable(request,&available),"Read HTTP response size");if(!available)break;require(response.bytes.size()+available<=limit,"Response exceeds 1 MiB");auto offset=response.bytes.size();response.bytes.resize(offset+available);DWORD read=0;checked(WinHttpReadData(request,response.bytes.data()+offset,available,&read),"Read HTTP response");require(read!=0,"Card Vault response ended unexpectedly");response.bytes.resize(offset+read);}
  if(response.status<200||response.status>=300){std::string cause="Card Vault returned HTTP "+std::to_string(response.status);try{cause+=" - "+response.json().value("message",std::string("Request failed"));}catch(const Json::exception&){cause+=" - invalid service response";}throw HttpError(response.status,cause);}
  return response;
}
std::vector<unsigned char> read_file(const fs::path& path){auto size=fs::file_size(path);require(size>0&&size<=limit,"Card file must be between 1 byte and 1 MiB");std::ifstream input(path,std::ios::binary);std::vector<unsigned char> bytes(size);input.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(size));require(bool(input),"Cannot read card file");return bytes;}
void write_new(const fs::path& path,const std::vector<unsigned char>& data){fs::create_directories(path.parent_path());HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);require(file!=INVALID_HANDLE_VALUE,"Card destination already exists or cannot be created");DWORD count=0;BOOL wrote=WriteFile(file,data.data(),DWORD(data.size()),&count,nullptr);BOOL flushed=wrote&&count==data.size()?FlushFileBuffers(file):FALSE;DWORD cause=GetLastError();CloseHandle(file);require(wrote&&count==data.size()&&flushed,"Card write failed (Windows "+std::to_string(cause)+"). Incomplete destination retained; do not overwrite it.");}
void validate_card(const std::vector<unsigned char>& png){
  require(png.size()>33&&png.size()<=limit&&std::equal(png.begin(),png.begin()+8,std::array<unsigned char,8>{137,80,78,71,13,10,26,10}.begin()),"Invalid PNG card");
  auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);require(SUCCEEDED(initialized)||initialized==RPC_E_CHANGED_MODE,"Image decoder initialization failed");
  struct Apartment{bool owns;~Apartment(){if(owns)CoUninitialize();}} apartment{SUCCEEDED(initialized)};
  ComPtr<IWICImagingFactory> factory;require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"PNG decoder unavailable");
  ComPtr<IWICStream> stream;require(SUCCEEDED(factory->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()),DWORD(png.size()))),"PNG stream failed");
  ComPtr<IWICBitmapDecoder> decoder;require(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder)),"PNG decode failed");
  ComPtr<IWICBitmapFrameDecode> frame;require(SUCCEEDED(decoder->GetFrame(0,&frame)),"PNG frame missing");UINT width=0,height=0;require(SUCCEEDED(frame->GetSize(&width,&height))&&width&&height&&width<=2048&&height<=2048,"PNG dimensions exceed 2048 pixels");
  ComPtr<IWICFormatConverter> converter;require(SUCCEEDED(factory->CreateFormatConverter(&converter))&&SUCCEEDED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"PNG pixel conversion failed");
  std::vector<unsigned char> pixels(size_t(width)*height*4);require(SUCCEEDED(converter->CopyPixels(nullptr,width*4,UINT(pixels.size()),pixels.data())),"PNG pixels unavailable");
  std::array<unsigned char,15> header{};size_t bits=0,opaque=0;
  for(UINT row=0;row<height;++row)for(UINT x=0;x<width;++x){auto offset=(size_t(height-1-row)*width+x)*4;if(!pixels[offset+3])continue;++opaque;for(int channel=0;channel<3&&bits<120;++channel,++bits)header[bits/8]|=(pixels[offset+channel]&1)<<(bits%8);}
  auto u32=[&](size_t at){return uint32_t(header[at])<<24|uint32_t(header[at+1])<<16|uint32_t(header[at+2])<<8|header[at+3];};
  unsigned version=unsigned(header[4])<<8|header[5];size_t skip=version==1?30:50;
  require(bits==120&&u32(0)==42&&(version==1||version==2)&&header[10]==1&&opaque>skip&&u32(6)>0&&u32(6)<=(opaque-skip)*3/8,"PNG does not contain a supported MT2 character card");
}
void save_connection(const fs::path& root,const Json& value){auto text=value.dump();auto encrypted=protected_bytes({text.begin(),text.end()},true);fs::create_directories(root);auto temporary=root/("connection-"+random_id()+".tmp");write_new(temporary,encrypted);checked(MoveFileExW(temporary.c_str(),(root/L"connection.bin").c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH),"Save protected connection");}
void preview_rgba(const std::vector<unsigned char>& png,const fs::path& destination){
  if(fs::exists(destination))return;
  auto initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);require(SUCCEEDED(initialized)||initialized==RPC_E_CHANGED_MODE,"Preview decoder initialization failed");
  struct Apartment{bool owns;~Apartment(){if(owns)CoUninitialize();}} apartment{SUCCEEDED(initialized)};
  ComPtr<IWICImagingFactory> factory;require(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory))),"Preview decoder unavailable");
  ComPtr<IWICStream> stream;require(SUCCEEDED(factory->CreateStream(&stream))&&SUCCEEDED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()),DWORD(png.size()))),"Preview stream failed");
  ComPtr<IWICBitmapDecoder> decoder;require(SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(),nullptr,WICDecodeMetadataCacheOnLoad,&decoder)),"Preview PNG decode failed");
  ComPtr<IWICBitmapFrameDecode> frame;require(SUCCEEDED(decoder->GetFrame(0,&frame)),"Preview frame missing");UINT source_width=0,source_height=0;require(SUCCEEDED(frame->GetSize(&source_width,&source_height))&&source_width&&source_height&&source_width<=2048&&source_height<=2048,"Preview dimensions invalid");
  float scale=(std::min)(1.0f,(std::min)(320.0f/source_width,480.0f/source_height));uint32_t width=(std::max)(1u,uint32_t(source_width*scale)),height=(std::max)(1u,uint32_t(source_height*scale));
  ComPtr<IWICBitmapScaler> scaler;require(SUCCEEDED(factory->CreateBitmapScaler(&scaler))&&SUCCEEDED(scaler->Initialize(frame.Get(),width,height,WICBitmapInterpolationModeFant)),"Preview resize failed");
  ComPtr<IWICFormatConverter> converter;require(SUCCEEDED(factory->CreateFormatConverter(&converter))&&SUCCEEDED(converter->Initialize(scaler.Get(),GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom)),"Preview conversion failed");
  std::vector<unsigned char> bytes(16+size_t(width)*height*4);std::memcpy(bytes.data(),"M2RGBA1",8);std::memcpy(bytes.data()+8,&width,4);std::memcpy(bytes.data()+12,&height,4);
  require(SUCCEEDED(converter->CopyPixels(nullptr,width*4,UINT(bytes.size()-16),bytes.data()+16)),"Preview pixels unavailable");write_new(destination,bytes);
}
Json load_connection(const fs::path& root){auto path=root/L"connection.bin";if(!fs::exists(path))return Json::object();auto bytes=protected_bytes(read_file(path),false);return Json::parse(bytes.begin(),bytes.end());}
void open_browser(const std::string& url){require(url.starts_with(std::string(CARDVAULT_ORIGIN)+"/connect?code=")||url.starts_with(std::string(CARDVAULT_ORIGIN)+"/cards/"),"Browser destination is outside Card Vault");auto value=wide(url);auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr,L"open",value.c_str(),nullptr,nullptr,SW_SHOWNORMAL));require(result>32,"Could not open your browser");}
}
