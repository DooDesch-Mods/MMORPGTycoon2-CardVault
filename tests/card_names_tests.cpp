#include "card_names.h"
#include <iostream>
#include <stdexcept>
#include <fstream>

int main() {
    auto check = [](bool okay) { if (!okay) throw std::runtime_error("Card filename test failed"); };
    check(vault::card_basename("Druid Female") == "Druid Female");
    check(vault::card_basename("  Bard  ") == "Bard");
    check(vault::card_basename("../../Druid: Female?*") == "Druid Female");
    check(vault::card_basename(" . ") == "Character card");
    check(vault::card_basename("CON") == "Card CON");
    check(vault::card_basename("nul.txt") == "Card nul.txt");
    check(vault::card_basename("Lpt1") == "Card Lpt1");
    check(vault::card_basename("Grüner Wächter") == "Grüner Wächter");
    check(vault::card_basename(std::string(79,'a') + "ü").size() == 79);
    check(vault::card_basename(std::string(81,'a')).size() == 80);
    check(vault::card_basename("Druid\nFemale\tMage") == "Druid Female Mage");
    namespace fs=std::filesystem;
    auto root=fs::current_path()/"card-names-fixture";
    check(fs::create_directory(root));
    auto read=[](const fs::path& path){std::ifstream in(path,std::ios::binary);return std::string(std::istreambuf_iterator<char>(in),{});};
    std::string digest="abcdef0123456789";
    check(vault::readable_card_path(root,"Druid Female",digest,read)==root/"Druid Female.png");
    {std::ofstream(root/"Druid Female.png") << digest;}
    check(vault::readable_card_path(root,"Druid Female",digest,read)==root/"Druid Female.png");
    {std::ofstream(root/"Druid Female.png") << "existing card";}
    auto collision=root/"Druid Female - abcdef012345.png";
    check(vault::readable_card_path(root,"Druid Female",digest,read)==collision);
    check(read(root/"Druid Female.png")=="existing card");
    {std::ofstream(collision) << "another existing card";}
    bool rejected=false;
    try{vault::readable_card_path(root,"Druid Female",digest,read);}catch(const std::runtime_error&){rejected=true;}
    check(rejected && read(collision)=="another existing card");
    fs::remove(collision);fs::remove(root/"Druid Female.png");fs::remove(root);
    std::cout << "16 card filename and collision cases passed\n";
}
