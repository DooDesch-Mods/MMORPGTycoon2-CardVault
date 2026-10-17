#include "vault.h"
#include <array>
#include <atomic>
#include <fstream>
#include <cstring>

namespace vault::game {
namespace {
std::atomic<uintptr_t> image{0};
DWORD thread=0;
constexpr char supported[]="bbab2dc68babcc644d0a1679548bf1e334b6c52aea669c3f61f6cb9d45c1aaf6";
struct alignas(8) String{unsigned char bytes[32]{};};
void require(bool okay,const std::string& cause){if(!okay)throw std::runtime_error(cause);}
bool readable(const void* address,size_t size){MEMORY_BASIC_INFORMATION region{};if(!address||!VirtualQuery(address,&region,sizeof region)||region.State!=MEM_COMMIT||region.Protect&(PAGE_GUARD|PAGE_NOACCESS))return false;auto offset=uintptr_t(address)-uintptr_t(region.BaseAddress);return offset<=region.RegionSize&&size<=region.RegionSize-offset;}
template<class T> bool read(void* object,size_t offset,T& result){auto source=static_cast<unsigned char*>(object)+offset;if(!object||!readable(source,sizeof(T)))return false;std::memcpy(&result,source,sizeof(T));return true;}
bool text(void* object,size_t offset,std::string& value){const char* bytes=nullptr;size_t size=0;if(!read(object,offset,bytes)||!read(object,offset+8,size)||size>256||!readable(bytes,size+1)||bytes[size])return false;value.assign(bytes,size);return true;}
void guard(){require(image.load()!=0,"Unsupported game build. Native card operations are unavailable.");if(!thread)thread=GetCurrentThreadId();require(thread==GetCurrentThreadId(),"Card operation must run on the game thread");}
DWORD find_window(void** output){String name{};bool built=false;DWORD error=0;auto base=image.load();__try{reinterpret_cast<void(*)(void*,const char*)>(base+0x49e090)(&name,"info_charactertype");built=true;*output=reinterpret_cast<void*(*)(const void*)>(base+0x81aa90)(&name);}__except(EXCEPTION_EXECUTE_HANDLER){error=GetExceptionCode();}if(built){__try{reinterpret_cast<void(*)(void*)>(base+0x11bd1e0)(&name);}__except(EXCEPTION_EXECUTE_HANDLER){if(!error)error=GetExceptionCode();}}return error;}
DWORD string_method(uintptr_t rva,void* object,const char* value,void** output=nullptr){String input{};bool built=false;DWORD error=0;auto base=image.load();__try{reinterpret_cast<void(*)(void*,const char*)>(base+0x49e090)(&input,value);built=true;if(output)*output=reinterpret_cast<void*(*)(void*,const void*)>(base+rva)(object,&input);else reinterpret_cast<void(*)(void*,const void*)>(base+rva)(object,&input);}__except(EXCEPTION_EXECUTE_HANDLER){error=GetExceptionCode();}if(built){__try{reinterpret_cast<void(*)(void*)>(base+0x11bd1e0)(&input);}__except(EXCEPTION_EXECUTE_HANDLER){if(!error)error=GetExceptionCode();}}return error;}
DWORD method(uintptr_t rva,void* receiver){__try{reinterpret_cast<void(*)(void*)>(image.load()+rva)(receiver);}__except(EXCEPTION_EXECUTE_HANDLER){return GetExceptionCode();}return 0;}
void checked(DWORD code,const char* operation){require(!code,std::string(operation)+" failed (native exception "+std::to_string(code)+"). Result unknown; do not repeat automatically.");}
void* info(bool visible){
  void* window=nullptr;checked(find_window(&window),"Find character window");if(!window)return nullptr;
  uintptr_t table=0;require(read(window,0,table)&&table==image.load()+0x173a7f0,"Character window layout differs");
  uint8_t shown=0;require(read(window,0x45f,shown)&&shown<=1,"Character window visibility is unreadable");if(visible&&!shown)return nullptr;
  void* card=nullptr;checked(string_method(0x81d860,window,"stone_card",&card),"Find character card");
  for(int depth=0;card&&depth<16;++depth){require(read(card,0,table),"Character pane is unreadable");if(table==image.load()+0x1736770)return card;void* parent=nullptr;require(read(card,0x278,parent),"Character pane parent is unreadable");if(parent==card)break;card=parent;}
  return nullptr;
}
}
void initialize(){
  wchar_t name[32768]{};auto count=GetModuleFileNameW(nullptr,name,DWORD(std::size(name)));require(count>0&&count<std::size(name),"Cannot identify game executable");
  std::ifstream stream(fs::path(name),std::ios::binary|std::ios::ate);require(bool(stream)&&stream.tellg()>0&&stream.tellg()<100*1024*1024,"Cannot read supported game executable");auto size=size_t(stream.tellg());stream.seekg(0);std::vector<unsigned char> bytes(size);stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(size));require(bool(stream)&&hash(bytes)==supported,"Unsupported game executable. MT2 Card Vault requires the supported Steam build.");image=uintptr_t(GetModuleHandleW(nullptr));
}
fs::path card_directory(const fs::path& profile){
  guard();auto module=GetModuleHandleW(L"libphysfs.dll");require(module!=nullptr,"Game file system is unavailable");auto write=reinterpret_cast<const char*(*)()>(GetProcAddress(module,"PHYSFS_getWriteDir"));require(write!=nullptr,"Game write directory API is unavailable");auto directory=write();require(directory!=nullptr,"Game profile is not ready");auto actual=fs::weakly_canonical(utf8path(directory));auto expected=fs::weakly_canonical(profile/"game-data");auto mismatch=std::mismatch(expected.begin(),expected.end(),actual.begin(),actual.end());require(mismatch.first==expected.end()&&actual!=expected,"Game write directory is outside the active framework profile");return actual/"prefabs/characters";
}
std::string selected_character(){guard();auto view=info(true);if(!view)return {};void* character=nullptr;require(read(view,0x460,character),"Selected character is unreadable");if(!character)return {};std::string name;require(text(character,0xb0,name),"Selected character name is unreadable");return name;}
fs::path export_selected(const fs::path& profile){
  guard();auto view=info(true);require(view!=nullptr,"Open a character's information window before exporting.");auto name=selected_character();require(!name.empty(),"Select a character before exporting.");
  uint8_t busy=0;require(read(view,0x4b0,busy)&&busy==0,"A native character export is already in progress");
  std::string safe;for(unsigned char c:name){if(safe.size()>=40)break;safe+=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')?char(c):'_';}
  auto filename="cardvault-"+safe+"-"+random_id();auto destination=card_directory(profile)/utf8path(filename+".png");require(!fs::exists(destination),"Export destination already exists");
  checked(method(0x6840b0,view),"Start native character export");require(read(view,0x4b0,busy)&&busy==1,"The game cannot capture this character card right now. Open its card view and try again.");
  // The original asynchronous export uses this private filename after its screenshot is ready.
  checked(string_method(0x11bfce0,static_cast<unsigned char*>(view)+0x490,filename.c_str()),"Set unique export filename");
  std::string actual;require(text(view,0x490,actual)&&actual==filename,"Export filename was not acknowledged; result unknown");
  return destination;
}
void refresh_library(){
  guard();auto view=info(false);if(!view)return;void* prefabs=nullptr;require(read(view,0x3b8,prefabs),"Character library is unreadable");if(!prefabs)return;uintptr_t table=0;require(read(prefabs,0,table)&&table==image.load()+0x1731f40,"Character library layout differs");checked(method(0x68e7e0,prefabs),"Refresh native card library");
}
}
