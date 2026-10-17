#include "vault.h"
#include "card_names.h"
#include "mt2fw/plugin.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <chrono>

namespace vault {
namespace {
using Clock=std::chrono::steady_clock;
const MT2FW_Host* host=nullptr;
fs::path profile,data_root;
std::mutex gate;
std::condition_variable wake;
std::thread worker;
bool stopping=false,window=false;
uint64_t render_token=0,update_token=0,input_token=0;
struct Job{std::string kind;Json args=Json::object();};
std::deque<Job> jobs;
struct State{
  bool ready=false,busy=true,connected=false,refresh=false,export_requested=false,open_requested=false,names_dirty=false,names_refresh=false;
  std::string status="Starting Card Vault...",error,user,code,selected_character;
  fs::path cards;
  Json remote=Json::array(),local=Json::array(),preview;
  int offset=0,next=-1;
  std::string type,tab="community",query,selected_id,selected_path,image_id,image_path,imported_id;
  int local_offset=0;
  std::string web_import,game_id;
  Json known_cards=Json::object();
};
State state;
std::deque<Json> errors;
void failure(const std::string& cause){std::lock_guard lock(gate);if(state.error!=cause)errors.push_back({{"level","error"},{"message","Card Vault: "+cause}});state.error=cause;state.status="Action failed";state.busy=false;}
void status(const std::string& message){std::lock_guard lock(gate);state.status=message;state.error.clear();}
Json request(const std::string& operation,const Json& args=Json::object()){
  if(!host)throw std::runtime_error("Framework host is unavailable");
  thread_local std::vector<char> output(4*1024*1024);
  const auto encoded=args.dump();auto size=host->request(host->context,operation.c_str(),encoded.c_str(),output.data(),output.size());
  if(!size||size>output.size())throw std::runtime_error("Framework request result is unavailable; do not repeat mutations");
  auto result=Json::parse(output.data());if(!result.value("ok",false))throw std::runtime_error(result.value("error",std::string("Framework request failed")));return result.at("result");
}
void enqueue(std::string kind,Json args=Json::object()){
  std::lock_guard lock(gate);if(state.busy||stopping)return;state.busy=true;state.error.clear();state.status="Working...";jobs.push_back({std::move(kind),std::move(args)});wake.notify_one();
}
std::vector<unsigned char> body(const Json& value){auto text=value.dump();return {text.begin(),text.end()};}
void remember_card(const fs::path& path,const Json& info){
  auto name=info.value("name",std::string());if(name.empty())return;
  Json value={{"name",name},{"type",info.value("type",std::string("Character"))},{"bytes",fs::file_size(path)},{"modified",fs::last_write_time(path).time_since_epoch().count()}};
  std::lock_guard lock(gate);state.known_cards[utf8text(path.filename())]=std::move(value);state.names_dirty=true;
}
fs::path card_destination(const fs::path& directory,const std::string& name,const std::string& digest){
  return readable_card_path(directory,name,digest,[](const fs::path& path){return hash(read_file(path));});
}
bool legacy_card(const fs::path& path){
  auto stem=utf8text(path.stem());
  if(!stem.starts_with("cardvault-"))return false;
  auto key=stem.substr(10),remote_key=key;
  auto dash=key.rfind('-');
  if(dash!=std::string::npos&&key.size()-dash-1==12&&key.substr(dash+1).find_first_not_of("0123456789abcdef")==std::string::npos)remote_key=key.substr(0,dash);
  bool remote=remote_key.size()>=20&&remote_key.size()<=128&&remote_key.front()=='c'&&remote_key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789")==std::string::npos;
  bool exported=dash!=std::string::npos&&key.size()-dash-1==24&&key.substr(dash+1).find_first_not_of("0123456789abcdef")==std::string::npos;
  return remote||exported;
}
fs::path readable_card(const fs::path& source,const Json& info){
  if(!legacy_card(source))return source;
  auto digest=hash(read_file(source)),name=info.value("name",std::string());
  if(name.empty())throw std::runtime_error("The card name is unavailable. The original file was kept.");
  auto destination=card_destination(source.parent_path(),name,digest),retained=destination;
  if(destination==source)return source;
  if(fs::exists(destination)){
    retained=data_root/"previous-filenames"/utf8path(random_id()+"-"+utf8text(source.filename()));
    fs::create_directories(retained.parent_path());
  }
  if(!MoveFileExW(source.c_str(),retained.c_str(),MOVEFILE_WRITE_THROUGH))throw std::runtime_error("Card rename failed (Windows "+std::to_string(GetLastError())+"). The original file was kept.");
  if(hash(read_file(destination))!=digest||hash(read_file(retained))!=digest)throw std::runtime_error("Card rename readback differs. Retained files require inspection before retrying.");
  remember_card(destination,info);
  {std::lock_guard lock(gate);state.known_cards[utf8text(source.filename())]={{"renamed_to",utf8text(destination.filename())},{"retained_path",utf8text(retained)},{"sha256",digest}};state.names_dirty=true;
    errors.push_back({{"level","info"},{"message","Card filename updated: "+utf8text(source.filename())+" -> "+utf8text(destination.filename())}});}
  return destination;
}
void index_card_names(const std::string& token){
  fs::path directory;Json known;{std::lock_guard lock(gate);directory=state.cards;known=state.known_cards;}
  if(directory.empty()||!fs::exists(directory))return;
  std::vector<fs::path> legacy;
  for(auto& file:fs::directory_iterator(directory))if(file.is_regular_file()&&!file.is_symlink()&&file.path().extension()==L".png"&&legacy_card(file.path())){
    if(legacy.size()>=200)throw std::runtime_error("More than 200 legacy Card Vault files need indexing. No further files were changed.");legacy.push_back(file.path());
  }
  size_t changed=0;
  for(auto& path:legacy){
    auto filename=utf8text(path.filename());Json info=known.value(filename,Json::object());
    if(info.contains("name")&&(info.value("bytes",uint64_t(0))!=fs::file_size(path)||info.value("modified",int64_t(0))!=fs::last_write_time(path).time_since_epoch().count()))info=Json::object();
    if(!info.contains("name")){
      if(token.empty())continue;
      auto bytes=read_file(path);validate_card(bytes);
      info=http("POST","/api/mod/v1/uploads/preview",token,bytes,true).json();
      if(info.at("sha256")!=hash(bytes))throw std::runtime_error("Legacy card name preview hash differs. The file was kept.");
    }
    readable_card(path,info);++changed;
  }
  if(changed){std::lock_guard lock(gate);state.refresh=true;state.names_refresh=true;state.status="Updating card names in the game library...";}
}
void local_cards(){
  fs::path directory;Json known;{std::lock_guard lock(gate);directory=state.cards;known=state.known_cards;}
  if(directory.empty())throw std::runtime_error("The game profile is not ready");
  Json list=Json::array();if(fs::exists(directory))for(auto& file:fs::directory_iterator(directory)){
    if(list.size()>=200)break;if(file.is_regular_file()&&file.path().extension()==L".png"&&!file.is_symlink()){
      auto filename=utf8text(file.path().filename()),name=utf8text(file.path().stem()),type=std::string("Character");
      if(name.starts_with("cardvault-")){
        name=name.substr(10);auto suffix=name.rfind('-');
        if(suffix!=std::string::npos&&name.size()-suffix-1==24)name=name.substr(0,suffix);else name="Imported character card";
        std::replace(name.begin(),name.end(),'_',' ');
      }
      if(known.contains(filename)){
        auto& info=known[filename];if(info.is_object()&&info.value("bytes",uint64_t(0))==file.file_size()&&info.value("modified",int64_t(0))==file.last_write_time().time_since_epoch().count()){name=info.value("name",name);type=info.value("type",type);}
      }
      list.push_back({{"name",name},{"type",type},{"path",utf8text(file.path())}});
    }
  }
  std::lock_guard lock(gate);state.local=std::move(list);
}
void run_worker(){
  std::string token,device,background_error;
  auto poll_at=Clock::time_point::max();
  auto game_poll_at=Clock::time_point::max();
  try{
    game::initialize();
    auto stored=load_connection(data_root);
    if(stored.is_object()&&stored.value("origin",std::string())==CARDVAULT_ORIGIN){
      token=stored.value("token",std::string());
      if(!token.empty()){
        auto me=http("GET","/api/mod/v1/me",token).json();
        std::lock_guard lock(gate);state.connected=true;state.user=me.at("user").value("name",std::string("Card Vault user"));
      }
    }
    {std::lock_guard lock(gate);state.ready=true;state.busy=false;state.status=state.connected?"Connected":"Connect your Card Vault account to begin.";if(state.connected){jobs.push_back({"browse"});game_poll_at=Clock::now();}}
  }catch(const std::exception& error){failure(error.what());std::lock_guard lock(gate);state.ready=true;}
  while(true){
    Job job;
    {
      std::unique_lock lock(gate);
      wake.wait_until(lock,(std::min)(poll_at,game_poll_at),[]{return stopping||!jobs.empty();});
      if(stopping)break;
      if(!jobs.empty()){job=std::move(jobs.front());jobs.pop_front();}
      else if(!device.empty()&&Clock::now()>=poll_at)job.kind="poll";
      else if(!token.empty()&&Clock::now()>=game_poll_at&&!state.busy)job.kind="game_poll";
      else {if(Clock::now()>=game_poll_at)game_poll_at=Clock::now()+std::chrono::seconds(1);continue;}
      if(job.kind!="game_poll")state.busy=true;
    }
    try{
      if(job.kind=="connect"){
        auto result=http("POST","/api/mod/v1/device",{},body(Json::object())).json();
        device=result.at("device_code");poll_at=Clock::now()+std::chrono::seconds(5);
        {std::lock_guard lock(gate);state.code=result.at("user_code");state.status="Approve this code in your browser, then return to the game.";}
        open_browser(result.at("verification_uri_complete"));
      }else if(job.kind=="poll"){
        auto result=http("POST","/api/mod/v1/token",{},body({{"device_code",device}})).json();
        if(result.at("status")=="authorization_pending")poll_at=Clock::now()+std::chrono::seconds(5);
        else{
          token=result.at("access_token");save_connection(data_root,{{"origin",CARDVAULT_ORIGIN},{"token",token}});device.clear();poll_at=Clock::time_point::max();game_poll_at=Clock::now();
          std::lock_guard lock(gate);state.connected=true;state.user=result.at("user").at("name");state.code.clear();state.status="Connected as "+state.user;jobs.push_back({"index"});jobs.push_back({"browse"});
        }
      }else if(job.kind=="disconnect"){
        device.clear();poll_at=Clock::time_point::max();game_poll_at=Clock::time_point::max();
        if(!token.empty())http("DELETE","/api/mod/v1/connection",token);
        save_connection(data_root,Json::object());token.clear();
        std::lock_guard lock(gate);state.connected=false;state.user.clear();state.code.clear();state.remote=Json::array();state.preview=Json();state.image_id.clear();state.image_path.clear();state.selected_id.clear();state.status="Disconnected";
      }else if(job.kind=="game_poll"){
        game_poll_at=Clock::now()+std::chrono::seconds(5);
        auto result=http("POST","/api/mod/v1/game/poll",token,body(Json::object())).json();
        std::lock_guard lock(gate);state.game_id=result.at("game_id");
        if(!background_error.empty()){errors.push_back({{"level","info"},{"message","Card Vault website import connection recovered"}});background_error.clear();}
        if(!result.at("import").is_null()){
          auto imported=result.at("import");auto id=imported.at("id").get<std::string>();
          if(id.size()!=36||id.find_first_not_of("0123456789abcdef-")!=std::string::npos)throw std::runtime_error("Invalid website import request identity");
          jobs.push_back({"import",{{"id",imported.at("card_id")},{"web_id",id},{"card",imported.at("card")}}});state.busy=true;
        }
      }else if(job.kind=="ack"){
        auto result=http("POST","/api/mod/v1/game/imports/"+job.args.at("id").get<std::string>()+"/ack",token,body({{"status",job.args.at("status")},{"message",job.args.at("message")}})).json();
        if(!result.value("acknowledged",false))throw std::runtime_error("Website import acknowledgement was not confirmed");
      }else if(job.kind=="index"){
        index_card_names(token);local_cards();
      }else if(job.kind=="browse"){
        int offset=job.args.value("offset",0);auto type=job.args.value("type",std::string());
        auto query=job.args.value("query",std::string());
        std::string encoded;for(unsigned char c:query){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_')encoded+=char(c);else{constexpr char hex[]="0123456789ABCDEF";encoded+='%';encoded+=hex[c>>4];encoded+=hex[c&15];}}
        auto result=http("GET","/api/mod/v1/cards?offset="+std::to_string(offset)+(type.empty()?"":"&type="+type)+"&q="+encoded,token).json();
        std::lock_guard lock(gate);state.remote=result.at("cards");state.offset=offset;state.next=result.at("next_offset").is_null()?-1:result.at("next_offset").get<int>();state.type=type;state.status="Community cards";
        bool retained=false;for(auto& card:state.remote)if(card.at("id")==state.selected_id)retained=true;
        if(!retained){state.selected_id.clear();state.image_id.clear();state.image_path.clear();}
      }else if(job.kind=="remote_preview"){
        auto id=job.args.at("id").get<std::string>();if(id.empty()||id.size()>128||id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" )!=std::string::npos)throw std::runtime_error("Invalid card identity");
        auto result=http("GET","/api/mod/v1/cards/"+id+"/preview",token);validate_card(result.bytes);auto digest=hash(result.bytes);if(result.sha256!=digest)throw std::runtime_error("Card preview hash differs");
        auto relative="cache/"+digest+".png";if(!fs::exists(data_root/utf8path(relative)))write_new(data_root/utf8path(relative),result.bytes);
        std::lock_guard lock(gate);state.image_id=digest;state.image_path=relative;state.status="Ready to import";
      }else if(job.kind=="import"){
        std::string id=job.args.at("id");if(id.empty()||id.size()>128||id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-" )!=std::string::npos)throw std::runtime_error("Invalid card identity");
        auto result=http("GET","/api/mod/v1/cards/"+id+"/download",token);validate_card(result.bytes);auto digest=hash(result.bytes);
        if(result.sha256!=digest)throw std::runtime_error("Card download hash differs. No card was imported.");
        fs::path directory;{std::lock_guard lock(gate);directory=state.cards;}
        if(directory.empty())throw std::runtime_error("The game profile is not ready");
        Json info;
        if(job.args.contains("card"))info=job.args.at("card");
        else {std::lock_guard lock(gate);for(auto& card:state.remote)if(card.at("id")==id){info=card;break;}}
        if(!info.is_object()||info.value("name",std::string()).empty())throw std::runtime_error("Card details are unavailable. Select the card again before importing.");
        auto destination=card_destination(directory,info.at("name"),digest);
        if(!fs::exists(destination))write_new(destination,result.bytes);
        if(hash(read_file(destination))!=digest)throw std::runtime_error("Imported card readback differs");
        if(info.is_object())remember_card(destination,info);
        std::string relative;
        if(job.args.contains("web_id")){relative="cache/"+digest+".png";if(!fs::exists(data_root/utf8path(relative)))write_new(data_root/utf8path(relative),result.bytes);}
        local_cards();std::lock_guard lock(gate);state.refresh=true;state.names_refresh=false;state.imported_id=id;state.web_import=job.args.value("web_id",std::string());state.open_requested=!state.web_import.empty();state.status="Card saved. Updating the game library...";
        if(!state.web_import.empty()){
          state.remote=Json::array({job.args.at("card")});state.selected_id=id;state.tab="community";state.offset=0;state.next=-1;state.type.clear();state.query=job.args.at("card").at("name");state.image_id=digest;state.image_path=relative;
        }
      }else if(job.kind=="local"){
        index_card_names(token);local_cards();status("Choose a local card to preview before uploading.");
      }else if(job.kind=="preview"||job.kind=="exported"){
        fs::path path=utf8path(job.args.at("path").get<std::string>());
        if(job.kind=="exported"){
          bool complete=false;std::string retained;
          for(int attempt=0;attempt<50;++attempt){
            {std::unique_lock lock(gate);if(wake.wait_for(lock,std::chrono::milliseconds(200),[]{return stopping;}))return;}
            if(fs::exists(path)){
              try{auto bytes=read_file(path);validate_card(bytes);complete=true;break;}catch(const std::exception& error){retained=error.what();}
            }
          }
          if(!complete)throw std::runtime_error("Native export was not confirmed within ten seconds. Result unknown; check local cards before retrying."+(retained.empty()?"":" "+retained));
          local_cards();
        }
        auto bytes=read_file(path);validate_card(bytes);auto preview=http("POST","/api/mod/v1/uploads/preview",token,bytes,true).json();
        if(preview.at("sha256")!=hash(bytes))throw std::runtime_error("Upload preview hash differs");
        auto previous_path=path;path=readable_card(path,preview);
        auto digest=hash(bytes),relative="cache/"+digest+".png";if(!fs::exists(data_root/utf8path(relative)))write_new(data_root/utf8path(relative),bytes);
        remember_card(path,preview);local_cards();
        preview["path"]=utf8text(path);std::lock_guard lock(gate);state.preview=std::move(preview);state.selected_path=utf8text(path);state.image_id=digest;state.image_path=relative;state.tab="local";state.status="Preview ready. Confirm to publish this card.";
        if(path!=previous_path){state.refresh=true;state.names_refresh=true;}
      }else if(job.kind=="upload"){
        auto bytes=read_file(utf8path(job.args.at("path").get<std::string>()));auto digest=hash(bytes);
        if(job.args.at("sha256")!=digest)throw std::runtime_error("The card changed after preview. Preview it again before uploading.");
        auto result=http("POST","/api/mod/v1/uploads",token,bytes,true,digest).json();
        status(result.value("already_exists",false)?"This card is already in Card Vault.":"Card published in Card Vault.");
        std::lock_guard lock(gate);state.preview=Json();
      }else if(job.kind=="open")open_browser(std::string(CARDVAULT_ORIGIN)+"/cards/"+job.args.at("slug").get<std::string>());
      else throw std::runtime_error("Unknown Card Vault job");
      if(job.kind!="game_poll"){std::lock_guard lock(gate);state.busy=!jobs.empty()||state.refresh||state.export_requested;}
    }catch(const HttpError& error){
      if(error.status==401){
        token.clear();device.clear();poll_at=Clock::time_point::max();game_poll_at=Clock::time_point::max();
        {std::lock_guard lock(gate);state.connected=false;state.user.clear();state.code.clear();state.image_id.clear();state.image_path.clear();}
        failure(std::string(error.what())+" Connect with Discord again.");
      }else if(job.kind=="game_poll"){
        if(background_error!=error.what()){background_error=error.what();std::lock_guard lock(gate);errors.push_back({{"level","warn"},{"message","Card Vault website import unavailable: "+background_error}});}
      }else{
        if(job.kind=="import"&&job.args.contains("web_id")){std::lock_guard lock(gate);jobs.push_front({"ack",{{"id",job.args.at("web_id")},{"status","unknown"},{"message",std::string(error.what()).substr(0,200)}}});wake.notify_one();}
        failure(error.what());
      }
    }catch(const std::exception& error){
      if(job.kind=="game_poll"){
        if(background_error!=error.what()){background_error=error.what();std::lock_guard lock(gate);errors.push_back({{"level","warn"},{"message","Card Vault website import unavailable: "+background_error}});}continue;
      }
      if(job.kind=="import"&&job.args.contains("web_id")){std::lock_guard lock(gate);jobs.push_front({"ack",{{"id",job.args.at("web_id")},{"status","unknown"},{"message",std::string(error.what()).substr(0,200)}}});wake.notify_one();}
      if(job.kind=="poll"){device.clear();poll_at=Clock::time_point::max();}
      if(job.kind=="poll"){std::lock_guard lock(gate);state.code.clear();}
      failure(error.what());
    }
  }
}
Clock::time_point search_at=Clock::time_point::max();
std::string field(const Json& item,const char* name,const std::string& fallback=""){return item.contains(name)&&item[name].is_string()?item[name].get<std::string>():fallback;}
void MT2FW_CALL update(void*,const char*,size_t){
  try{
    State snapshot;std::deque<Json> pending;
    {std::lock_guard lock(gate);snapshot=state;pending.swap(errors);}
    for(auto& error:pending)request("log",error);
    if(!snapshot.ready)return;
    if(snapshot.names_dirty){
      Json known;{std::lock_guard lock(gate);known=state.known_cards;state.names_dirty=false;}
      try{request("storage.set",{{"scope","profile"},{"key","card_names"},{"value",known}});}
      catch(const std::exception& error){request("log",{{"level","warn"},{"message",std::string("Card Vault could not retain card names; filenames remain available after restart: ")+error.what()}});}
    }
    if(snapshot.open_requested){window=true;std::lock_guard lock(gate);state.open_requested=false;}
    if(snapshot.connected&&!snapshot.busy&&Clock::now()>=search_at){search_at=Clock::time_point::max();enqueue("browse",{{"offset",0},{"type",snapshot.type},{"query",snapshot.query}});}

    if(snapshot.cards.empty()){
      auto directory=game::card_directory(profile);std::lock_guard lock(gate);state.cards=directory;state.busy=true;jobs.push_back({"index"});wake.notify_one();
    }
    if(snapshot.refresh){
      {std::lock_guard lock(gate);state.refresh=false;state.names_refresh=false;}
      try{
        game::refresh_library();status(snapshot.names_refresh?"Card names updated in the game library.":"Imported into your character library.");
        std::lock_guard lock(gate);state.web_import.clear();if(!snapshot.web_import.empty()){jobs.push_back({"ack",{{"id",snapshot.web_import},{"status","confirmed"},{"message","Added to your game's character library."}}});wake.notify_one();}state.busy=!jobs.empty();
      }catch(const std::exception& error){
        if(!snapshot.web_import.empty()){std::lock_guard lock(gate);state.web_import.clear();jobs.push_back({"ack",{{"id",snapshot.web_import},{"status","unknown"},{"message",std::string(error.what()).substr(0,200)}}});wake.notify_one();}throw;
      }
    }
    if(snapshot.export_requested){
      {std::lock_guard lock(gate);state.export_requested=false;}
      auto destination=game::export_selected(profile);
      std::lock_guard lock(gate);jobs.push_back({"exported",{{"path",utf8text(destination)}}});wake.notify_one();
    }
  }catch(const std::exception& error){failure(error.what());}
}
void MT2FW_CALL input(void*,const char* bytes,size_t length){
  try{auto event=Json::parse(bytes,bytes+length);if(event.value("name",std::string())=="toggle")window=!window;}
  catch(const std::exception& error){failure(std::string("Input event: ")+error.what());}
}
void MT2FW_CALL render(void*,const char*,size_t){
  try{
    auto launch=request("ui.app",{{"id","main"},{"title","Card Vault"}});
    if(launch.value("open_requested",false))window=true;
    if(!window)return;
    State snapshot;{std::lock_guard lock(gate);snapshot=state;}
    bool connected=snapshot.connected, community=snapshot.tab=="community", local=!community;
    Json widgets=Json::array();
    auto widget=[&](const char* type,const std::string& id,int x,int y,int width,int height,const std::string& text,bool visible=true,int size=16){
      Json item={{"type",type},{"id",id},{"x",x},{"y",y},{"width",width},{"height",height},{"text",text},{"visible",visible},{"size",size}};
      widgets.push_back(std::move(item));
    };
    auto text=[&](const std::string& id,int x,int y,int width,int height,const std::string& value,bool visible=true,int size=16,const char* tone=""){
      widget("text",id,x,y,width,height,value,visible,size);widgets.back()["tone"]=tone;
    };
    auto button=[&](const std::string& id,int x,int y,int width,int height,const std::string& label,bool visible=true,bool disabled=false,bool selected=false){
      widget("button",id,x,y,width,height,label,visible,15);widgets.back()["disabled"]=disabled;widgets.back()["selected"]=selected;
    };
    auto panel=[&](const std::string& id,int x,int y,int width,int height,const char* tone,bool visible=true){
      widget("panel",id,x,y,width,height,"",visible);widgets.back()["tone"]=tone;
    };
    // Fixed regions keep selection, preview and its action together across both tabs.
    panel("library_surface",16,72,340,450,"subtle",connected);
    panel("preview_surface",372,72,532,450,"preview",connected);
    panel("welcome_surface",16,72,888,450,"preview",!connected);
    panel("status_surface",16,540,888,44,"subtle");
    button("community",16,14,170,36,"Community",connected,snapshot.busy,community);widgets.back()["style"]="tab";
    button("local",194,14,170,36,"My exports",connected,snapshot.busy,local);widgets.back()["style"]="tab";
    text("account",620,24,150,22,connected?snapshot.user:"",connected,14,"muted");
    button("disconnect",788,14,116,36,"Disconnect",connected,snapshot.busy);widgets.back()["style"]="quiet";
    text("intro",48,118,720,42,"Your next character starts here",!connected,28);
    text("connect_help",48,184,660,88,"Explore community cards, add characters to your library and share your own creations.\n\nConnect with Discord in your browser to get started.",!connected,17);
    button("connect",48,304,250,40,snapshot.busy?"Connecting...":"Connect with Discord",!connected&&snapshot.code.empty(),snapshot.busy||!snapshot.ready);
    text("connect_note",48,362,660,48,"Discord sign-in stays in your browser. You can disconnect this game at any time.",!connected&&snapshot.code.empty(),14,"muted");
    text("code_heading",48,292,720,30,"Approve this game in your browser",!connected&&!snapshot.code.empty(),22);
    text("code",48,334,450,40,snapshot.code,!connected&&!snapshot.code.empty(),28);
    text("code_help",48,388,700,44,"Check that the website shows this code. Return here after approval.",!connected&&!snapshot.code.empty(),16,"muted");
    button("cancel",48,454,180,34,"Cancel connection",!connected&&!snapshot.code.empty(),snapshot.busy);widgets.back()["style"]="quiet";
    text("search_title",28,86,316,20,"Find a character",connected&&community,15);
    widget("input","search",28,112,316,34,"",connected&&community,16);widgets.back()["value"]=snapshot.query;
    const char* types[]={"All","Class","Monster","Boss"};
    for(int i=0;i<4;++i){button(std::string("filter")+types[i],28+i*80,154,76,30,types[i],connected&&community,snapshot.busy,std::string(types[i])=="All"?snapshot.type.empty():snapshot.type==types[i]);widgets.back()["style"]="tab";}
    button("export",28,86,202,36,snapshot.busy?"Working...":"Export current character",connected&&local,snapshot.busy);
    button("refresh_local",238,86,106,36,"Refresh",connected&&local,snapshot.busy);widgets.back()["style"]="quiet";
    text("export_help",28,134,316,48,"Open a character's card in the game, then export it here.",connected&&local,15,"muted");
    int count=community?int(snapshot.remote.size()):(std::min)(10,(std::max)(0,int(snapshot.local.size())-snapshot.local_offset));
    for(int i=0;i<10;++i){
      Json card;
      if(connected&&i<count)card=community?snapshot.remote[i]:snapshot.local[size_t(snapshot.local_offset+i)];
      std::string label=card.is_null()?"":field(card,"name");
      bool selected=!card.is_null()&&(community?snapshot.selected_id==field(card,"id"):snapshot.selected_path==field(card,"path"));
      button("row"+std::to_string(i),28,198+i*28,316,26,label,connected&&i<count,snapshot.busy,selected);widgets.back()["style"]="row";
    }
    text("empty",32,218,300,112,snapshot.busy?"Loading cards...":community?"No cards found.\nTry another name or choose All.":"No exports yet.\nOpen a character's card, then choose Export current character.",connected&&count==0,17,"muted");
    button("previous",28,480,80,30,"Previous",connected&&community,snapshot.busy||snapshot.offset==0);widgets.back()["style"]="quiet";
    text("page",118,488,72,22,"Page "+std::to_string(snapshot.offset/10+1),connected&&community,13,"muted");
    button("next",196,480,68,30,"Next",connected&&community,snapshot.busy||snapshot.next<0);widgets.back()["style"]="quiet";
    button("refresh",272,480,72,30,"Refresh",connected&&community,snapshot.busy);widgets.back()["style"]="quiet";
    button("local_previous",28,480,142,30,"Previous",connected&&local,snapshot.busy||snapshot.local_offset==0);widgets.back()["style"]="quiet";
    button("local_next",202,480,142,30,"Next",connected&&local,snapshot.busy||size_t(snapshot.local_offset+10)>=snapshot.local.size());widgets.back()["style"]="quiet";
    bool image=connected&&!snapshot.image_id.empty();
    widget("image","artwork",388,92,220,392,"",image);widgets.back()["path"]=snapshot.image_path;
    Json selected;
    if(community)for(auto& card:snapshot.remote)if(field(card,"id")==snapshot.selected_id){selected=card;break;}
    bool remote=connected&&community&&!selected.is_null(), preview=connected&&local&&!snapshot.preview.is_null();
    text("card_name",632,104,248,68,remote?field(selected,"name"):preview?field(snapshot.preview,"name"):"Choose a card",connected,24);
    text("card_meta",632,184,248,70,remote?field(selected,"type")+"\nby "+field(selected,"author","Unknown creator"):preview?field(snapshot.preview,"type"):community?"Select a character from the list to see its card.":"Select an export to check its card before sharing.",connected,16,"muted");
    text("preview_empty",400,242,194,100,"Your card preview\nappears here",connected&&!image,18,"muted");
    bool imported=remote&&snapshot.imported_id==snapshot.selected_id;
    bool duplicate=preview&&!snapshot.preview.at("duplicate").is_null();
    button("import",632,288,248,40,imported?"Added to library":"Import card",remote,snapshot.busy||imported);
    button("website",632,340,248,34,"View on website",remote,snapshot.busy);widgets.back()["style"]="quiet";
    button("upload",632,288,248,40,duplicate?"Already published":"Publish this card",preview,snapshot.busy||duplicate);
    text("transfer_help",632,398,248,90,remote?"Adds a prefab to your library. Apply it from the game's Prefabs tab.":preview?"This card will be public on Card Vault as "+snapshot.user+".":"",remote||preview,15,"muted");
    text("status",28,554,860,24,snapshot.error.empty()?snapshot.status:"",true,14,"muted");
    text("error",28,548,860,34,snapshot.error,!snapshot.error.empty(),14,"error");
    text("game_id",632,494,248,20,snapshot.game_id.empty()?"":"Website imports connected",connected,12,"muted");
    auto result=request("ui.native",{{"id","main"},{"open",window},{"width",920},{"height",600},{"widgets",std::move(widgets)}});
    if(!result.value("ready",false))return;
    window=result.value("open",window);
    if(result.at("values").contains("search")){
      std::lock_guard lock(gate);state.query=result["values"]["search"].get<std::string>();search_at=Clock::now()+std::chrono::milliseconds(350);
    }
    for(auto& clicked:result.at("clicked")){
      auto id=clicked.get<std::string>();
      if(id.starts_with("filter"))id="type:"+id.substr(6);
      if(id.starts_with("row")){
        auto slot=size_t(std::stoul(id.substr(3)));
        if(community){if(slot>=snapshot.remote.size())throw std::runtime_error("Card selection expired");id="card:"+field(snapshot.remote[slot],"id");}
        else id="local:"+std::to_string(size_t(snapshot.local_offset)+slot);
      }
      auto browse=[&](int offset,const std::string& type){enqueue("browse",{{"offset",offset},{"type",type},{"query",snapshot.query}});};
      if(id=="connect")enqueue("connect");
      else if(id=="disconnect"||id=="cancel")enqueue("disconnect");
      else if(id=="community"||id.starts_with("type:")){
        {std::lock_guard lock(gate);state.tab="community";state.selected_id.clear();state.image_id.clear();state.image_path.clear();}
        auto type=id=="community"?snapshot.type:id.substr(5);browse(0,type=="All"?"":type);
      }else if(id=="local"||id=="refresh_local"){
        {std::lock_guard lock(gate);state.tab="local";state.image_id.clear();state.image_path.clear();state.preview=Json();state.selected_path.clear();}
        enqueue("local");
      }else if(id=="export"){
        std::lock_guard lock(gate);if(!state.busy){state.busy=true;state.export_requested=true;state.status="Exporting your character...";}
      }else if(id.starts_with("card:")){
        auto card=id.substr(5);{std::lock_guard lock(gate);state.selected_id=card;state.image_id.clear();state.image_path.clear();state.status="Loading preview...";}
        enqueue("remote_preview",{{"id",card}});
      }else if(id=="import")enqueue("import",{{"id",snapshot.selected_id}});
      else if(id=="upload")enqueue("upload",snapshot.preview);
      else if(id=="website"){for(auto& card:snapshot.remote)if(field(card,"id")==snapshot.selected_id)enqueue("open",{{"slug",card.at("slug")}});}
      else if(id=="refresh")browse(snapshot.offset,snapshot.type);
      else if(id=="previous")browse((std::max)(0,snapshot.offset-10),snapshot.type);
      else if(id=="next")browse(snapshot.next,snapshot.type);
      else if(id=="local_previous"||id=="local_next"){std::lock_guard lock(gate);state.local_offset=(std::max)(0,state.local_offset+(id=="local_next"?10:-10));}
      else if(id.starts_with("local:")){
        auto index=size_t(std::stoul(id.substr(6)));if(index>=snapshot.local.size())throw std::runtime_error("Local card selection expired");
        {std::lock_guard lock(gate);state.selected_path=field(snapshot.local[index],"path");state.image_id.clear();state.image_path.clear();state.preview=Json();}
        enqueue("preview",{{"path",snapshot.local[index].at("path")}});
      }
    }
  }catch(const std::exception& error){failure(error.what());}
}
}
}
extern "C" MT2FW_EXPORT int MT2FW_CALL MT2FW_PluginQuery(MT2FW_PluginInfo* info){if(!info||info->size<sizeof(*info)||info->abi_version!=MT2FW_ABI_VERSION)return 0;info->id="doodesch.cardvault";info->version="0.1.0";return 1;}
extern "C" MT2FW_EXPORT int MT2FW_CALL MT2FW_PluginLoad(const MT2FW_Host* api){
  using namespace vault;
  if(!api||api->size<sizeof(*api)||api->abi_version!=MT2FW_ABI_VERSION)return 0;
  host=api;
  try{
    HMODULE module=nullptr;if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&MT2FW_PluginLoad),&module))throw std::runtime_error("Cannot identify mod module");
    wchar_t filename[32768]{};auto length=GetModuleFileNameW(module,filename,DWORD(std::size(filename)));if(!length||length>=std::size(filename))throw std::runtime_error("Cannot identify mod directory");
    auto directory=fs::path(filename).parent_path();if(directory.filename()!=L"doodesch.cardvault"||directory.parent_path().filename()!=L"mods")throw std::runtime_error("Install Card Vault in the profile mods/doodesch.cardvault directory");
    profile=directory.parent_path().parent_path();data_root=profile/"data/doodesch.cardvault";
    auto known=request("storage.get",{{"scope","profile"},{"key","card_names"},{"default",Json::object()}});
    if(!known.is_object())throw std::runtime_error("Stored Card Vault names are invalid");state.known_cards=std::move(known);

    update_token=host->subscribe(host->context,"update",update,nullptr);render_token=host->subscribe(host->context,"render",render,nullptr);input_token=host->subscribe(host->context,"input",input,nullptr);
    if(!update_token||!render_token||!input_token)throw std::runtime_error("Card Vault callback registration failed");
    worker=std::thread(run_worker);return 1;
  }catch(const std::exception& error){failure(error.what());return 0;}
}
extern "C" MT2FW_EXPORT void MT2FW_CALL MT2FW_PluginShutdown(){
  using namespace vault;
  try{{std::lock_guard lock(gate);stopping=true;wake.notify_one();}if(worker.joinable())worker.join();if(host){for(auto id:{render_token,update_token,input_token})if(id)host->unsubscribe(host->context,id);}host=nullptr;}
  catch(const std::exception& error){OutputDebugStringA((std::string("error: Card Vault shutdown failed: ")+error.what()).c_str());}
}
