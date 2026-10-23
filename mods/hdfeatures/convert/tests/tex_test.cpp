// Unit tests of module T (tex) on synthetic data only. kVectors holds SHA-256 values of the Python spec's output
// (gtx2img, gxtex, hd2gc, stampbook, gen_stamp_table) on inputs made by a small LCG that this file repeats; the
// table is printed by `python -I -B tools/dev/golden_tex.py --vectors` (no game files needed). The untile port is
// also cross-checked against the CC0 tphd AddrLib (src/tex/addrlib_ref.inc) on random surfaces, and every parser
// must survive truncated and random input (Correction #1).
#include "test.hpp"

#include "../src/tex/addrlib_ref.hpp"
#include "../src/tex/addrlib_ref.inc"  // the tphd AddrLib reference: compiled here only
#include "hdf/io.hpp"
#include "hdf/source.hpp"
#include "hdf/tex.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using hdf::Bytes;
using hdf::u16;
using hdf::u32;
using hdf::u64;
using hdf::u8;
using hdf::View;
namespace io = hdf::io;
namespace tex = hdf::tex;
namespace ar = hdf::tex::addrlib_ref;

struct Vec {
    const char* name;
    const char* value;
};

// python -I -B tools/dev/golden_tex.py --vectors
const Vec kVectors[] = {
    {"deswizzle/1/1", "0243966e8b10a48df064e457ac5448bb7c0c6222b4288c4efecd6c32e2d3c24a"},
    {"deswizzle/1/2", "2f4ca58d2beffe7becbec010ae7a950c7bfd871bff29b728511a96cca1dd6321"},
    {"deswizzle/1/4", "1c13b6a10a0056f61b4de7f6b7665466240a1777989d22dfd7c96ea5d2c90c9f"},
    {"deswizzle/1/5", "8e1547605101ef93f2853e678919963a9065b8fe93012ce2a480952c31d50d27"},
    {"deswizzle/1/6", "936ddb2306f88b081e97911a7e0e9c727d379eff167e32725b4ec21d27378743"},
    {"deswizzle/2/1", "b8c076b56489e5f069de3a90b7086bd059ac6280975a04997a8513f38add7ea4"},
    {"deswizzle/2/2", "0a1153410763b040e5e36a395a1e9cbb429ce40782b4e22f74cbc6a4453167bc"},
    {"deswizzle/2/4", "c7fe0a9b5b6236177605d33763f5764f92f6b96f85d70b29ef25bf5d1aff8538"},
    {"deswizzle/2/5", "c7256580b1e2dc71c351f4d0b1bc4d6e004d3d0c1768f4a9616a7c10b41d5d5a"},
    {"deswizzle/2/6", "a3365603df89f6208bd972606660c5a51a6ce33cc2e1a7d796b92201962732d0"},
    {"deswizzle/7/1", "08b0c5105c0862d195025796f686938f0f04ca3beacad7fd32f05ac0a62eedb1"},
    {"deswizzle/7/2", "f7a71e357e54993258d9689d55fd8450c93dc769f66046b5788c24c6d42a637b"},
    {"deswizzle/7/4", "7e1032b32900f863c18b19d89f132bbcccf876191a437bbcbc1de179d81f251b"},
    {"deswizzle/7/5", "98902942dc310c7d0b3cc2cd10c56c041186fd2ffdab6856c4701416d81ba69f"},
    {"deswizzle/7/6", "612f95495fe9aa150c8a10fad14cce1424a374ce6c8e1bb67fb7379faee7b576"},
    {"deswizzle/8/1", "e25ae95d0c6a23f7898b83b5c3f257921c4150fbad6ea226f2d9d0c023f7feea"},
    {"deswizzle/8/2", "ebda9a6b0b6c150081ce9208e545da4adb049cb26718fc345eb29e73809a07e0"},
    {"deswizzle/8/4", "b9d006f2b8e6fbe931de2e07542fe173aa64136730e900ffacf93e477e9c9560"},
    {"deswizzle/8/5", "22e14f7d43a4fda3d1ded26aabb45d2ca10a8fa999cb7a2bc43189aca4fb36c4"},
    {"deswizzle/8/6", "8f9ce798c204142f411079a018e80f0cf58c8526ad85341754f525a632bed3fd"},
    {"deswizzle/1a/1", "2f436b8e02ebd3ac80367b7b0ce2e2602eea10fa710f181fbf76dc313ad36849"},
    {"deswizzle/1a/2", "2473d583e79a167087d2b10d580f7fad14c9ab0420871be6d40203b862882bf2"},
    {"deswizzle/1a/4", "0be2379cdb1dd69d325c6ac9960480e3edb83b374bdbf351ee30b79769d78822"},
    {"deswizzle/1a/5", "6a2d9293e2fb2fac5ed2b096335e20cfd0112f5eb46c7113267996f286f95689"},
    {"deswizzle/1a/6", "7fadf4071d2498a15a3f1f983e20b943ca057096ca596b4219f0d691dae575aa"},
    {"deswizzle/31/1", "5b5a73c862c3107b04464f40ecd7d835946836a9fdb5131d5b1681f57ad3d596"},
    {"deswizzle/31/2", "f4c3d0de2125065f97417c6658628382a13f7dbd5204628d2e870ac74e21dc49"},
    {"deswizzle/31/4", "74344f393c0a77c226d930fc0d9b7fbde8957acb27e3e05f15fc7d86f7753127"},
    {"deswizzle/31/5", "6b68d7d2b1d5f1d324fab3a5c4e18a31c807ad8704f3d025db162a2f53b29f5f"},
    {"deswizzle/31/6", "dc7ddcd29aca01c21604189eb5075104b98df8c0c07c4c2621aa82b3aa183645"},
    {"to_rgba/1", "719bb8d8617abce4146be9262899e611a9da02fb3c0c010b8c021e668b404d03"},
    {"to_rgba/2", "cb3e1a5752ab745629c26ca5991188893af1d160156a59bc2322c6bd05a409cf"},
    {"to_rgba/7", "0c5b380da3868e2cd9aa7f1c744ac7aa066fbfc46b28892648d31c4a4deb78dd"},
    {"to_rgba/8", "2accc7374b3e43afd5f5dc20af41b3973ce0cb3e8413ba09199d459ff972d153"},
    {"to_rgba/1a", "be478efc834e49423208569ae5213ea1be897567ed483f060548a95f8dbd80eb"},
    {"to_rgba/31", "3a4e4a5ee78a17691690f68c270246943c7d802eeb603842ed4a06ddd79d78e3"},
    {"gx_encode/0", "34630122f1d3dca97aac9930b98954a6b313967f54117481d70c6de3df515701"},
    {"gx_decode/0", "def5df06bb76d3843fe120e5637b43e3e20b8389f0c0fd1c2dce44384fd60afb"},
    {"gx_encode/1", "a6b9f7e874b8b1b8b3170c56a7be5940b1e7b62cde4e0ac422561e06815f2c5f"},
    {"gx_decode/1", "31b73f218311a9ed8ff6b9afe10922b0b0f4de7b97491d9f5a4145b7c66e47d5"},
    {"gx_encode/2", "56dd9f00dbd70b4458f45798285fb47acf53f40aaa4e923576b305c8779df631"},
    {"gx_decode/2", "b1198fb1385127aa8f5407da01d9ace63671a953b5a8514cbef714c3a9c84791"},
    {"gx_encode/3", "7fd2dd551054de825e73c7387f174fb70070cc21d5728ef04d3d1d870cce6be8"},
    {"gx_decode/3", "eace1b2e2abd8db4e2ceacf9572d9846456fb1c4c45e2d52a7244475d99d5c57"},
    {"gx_encode/4", "3a2963fce105ae62d095cc601638e8f65b5a115f5f6d6908a7a7340ecc2cf41a"},
    {"gx_decode/4", "480967f756848896751edf1ef151841590166f9e91357c102dc6ba0f8f492551"},
    {"gx_encode/5", "8deb0f42791aad452bebfd99beaf4f6c99c7b41e0631a5aa0f336658155c786d"},
    {"gx_decode/5", "8296200a7400b0aaaa1a30236494842149aeffa9e33f6184ef974738b2afa5be"},
    {"gx_encode/6", "f12344a238a8edac37e14c7066ad83a9be5e082973254c02034bc335bf1be529"},
    {"gx_decode/6", "5c90baf4f581f51ccbad1719980851f336ece6ea4c34fadee47488a174c8e75a"},
    {"gx_decode/14", "59fc78586f08ebf3377804dbc6aaa2cd5a593d601575f7ff365c91c8ecce2fc6"},
    {"bc1_to_cmpr", "611128e642182db9725232252d4f7a6d4d8a5cd4f887c1b8f07fff1ff63fd6a2"},
    {"box_downscale", "87e6cbd563bcce5c62e52ff6678ac34112e2e982baec24f81ff273c2ae9cfbee"},
    {"ui_encode/0", "a1ee3ffa9a235a94fa9237302d856b47746a6fac2ebc5e4b434df0002cf65f3f"},
    {"ui_decode/0", "a3dc249d3cd8dd920c6391704024ebaa7e5b7b8b93600f2f856b9bddbe262f7c"},
    {"ui_encode/1", "c78667f0470aa7d5331fdfd576b9387831b6b9abb75c876244d22b02d55c6444"},
    {"ui_decode/1", "8231a47dace9aaa6064c7f98c640c3a7b26ae7823e71058c17b7e0c1c3f47fa5"},
    {"ui_encode/2", "7a277e7b627646053da2a9fdc48b075bc92dca3ff82407cce8a63a0d37f2eaab"},
    {"ui_decode/2", "0b53aade7bd87d2cd664a8119216f9fa7783b3966bc682f806ce4d86d6bd13ea"},
    {"ui_encode/3", "c2343da4a8abf09fb9a81a4d846307148e012030fd4b3642d0e7816df0439035"},
    {"ui_decode/3", "245ef0d31ebe5c0ead03dba36a0d7b2bdd7248a7e02d9ea02e779177297d2490"},
    {"ui_encode/4", "53d76fc3b83c1d27954ede2553ef8c671a556d413b1cfc0f176ab5dc1f4fa1ea"},
    {"ui_decode/4", "5c548dc2768384980dfc11fcd30e89c603c46c6af01a1cf130208cb78d2ec89b"},
    {"ui_encode/5", "222dd5db65df5c03d19848367a14d95e411dc2c33c291cf3439a816626c6a8c2"},
    {"ui_decode/5", "faa53eb879f4ee1e2ddce858ed0330c2e0b62253218009558de8f3e9a9d78948"},
    {"ui_encode/6", "f12344a238a8edac37e14c7066ad83a9be5e082973254c02034bc335bf1be529"},
    {"ui_decode/6", "5c90baf4f581f51ccbad1719980851f336ece6ea4c34fadee47488a174c8e75a"},
    {"make_bti/i4/5", "I4|0|0|2ebebb76f8d8e4d5839e7e6a8dface0e32efdd09551698cd1257666c71152b9c|70e2ec74ec5afd3d93f3f221921a775e2107ea90d17f5fbcced6fee2a734082f"},
    {"choose/i4", "0|greyscale, alpha = intensity, 4-bit exact"},
    {"make_bti/i4/9", "I4|0|0|1c5dbb571a8f3ef69789c1295f387af3aa56adf7fb29f17344fb5543e4ef778d|70e2ec74ec5afd3d93f3f221921a775e2107ea90d17f5fbcced6fee2a734082f"},
    {"make_bti/i8/5", "I8|0|0|53a8d2170a0a498e0e38e11156c311663e612d7bc3ef83c138440cdb69c61f94|562bf3b7ed3843b121e723139adb63366ef25a998ee96a0c172b432b13425ccb"},
    {"choose/i8", "1|greyscale, alpha = intensity"},
    {"make_bti/i8/9", "I8|0|0|5e84967dbacb60020817089d9fe80a64c0d15d49430064a8c83271b91b0d8e3d|562bf3b7ed3843b121e723139adb63366ef25a998ee96a0c172b432b13425ccb"},
    {"make_bti/ia4/5", "IA4|0|0|7bec4a218d02162816832140968ec76033f0d4220a7546d5eea114389118686c|39c9fe0a794389a6718de2574e5d4c47fc4dd967dd0370b523f3edeb9612ff5e"},
    {"choose/ia4", "2|greyscale, 4-bit exact"},
    {"make_bti/ia4/9", "IA4|0|0|4e48f27cefa343ccca37975dd058176d8981ea6c7118da25a81870d70f2add0c|39c9fe0a794389a6718de2574e5d4c47fc4dd967dd0370b523f3edeb9612ff5e"},
    {"make_bti/ia8/5", "IA8|0|0|d5e072b53f82bfc2174ab698291aaf43e34d1a89fb8f0092ed0c280e99fbde8c|8fe7a14e52e044d9b712dab99f6b1c726c5a4b5e2dba76213a0feb3da997bc5d"},
    {"choose/ia8", "3|greyscale: lossless"},
    {"make_bti/ia8/9", "IA8|0|0|f6d4a8970ccdfa28d214e141d30be424737932728809efe3628af25c6d7a04d8|8fe7a14e52e044d9b712dab99f6b1c726c5a4b5e2dba76213a0feb3da997bc5d"},
    {"make_bti/ink/5", "RGBA8|0|0|0d1f0def965a20c5436a14d290a2703e6a38227c071b8173f6a65f059b58f024|9015788f7ddcb448db69a7d7e9221b91f7ecf4e8004cc15716085f30f783e482"},
    {"choose/ink", "5|flat ink #3080C8, alpha 0/255: one colour, no gradient (HD header RGB5A3)"},
    {"make_bti/ink/9", "RGBA8|0|0|9cfc5a6c7fc4a4397335f8e66cf8916f25b029549aa487c741e24b81f2f4c6cb|9015788f7ddcb448db69a7d7e9221b91f7ecf4e8004cc15716085f30f783e482"},
    {"make_bti/ink_ok/5", "RGB5A3|0|8|10a8508d379e65792f42f9143ba5b3da882c802cc940cb2192ea4b68e1b67879|418468bbd46838894c51d456684ff72ebd1f84c4c335ac6b844a7a7981608121"},
    {"choose/ink_ok", "5|flat ink #3184C6, alpha 0/255: one colour, no gradient (HD header RGB5A3)"},
    {"make_bti/ink_ok/9", "RGB5A3|0|8|6436d4095a2b2cea7c8883f93563e0d72e6d0839afb6d9a3ff365a47c275dc8c|418468bbd46838894c51d456684ff72ebd1f84c4c335ac6b844a7a7981608121"},
    {"make_bti/ink_far/5", "RGBA8|0|0|368bec70714aa846883bdb7f39fae467febcf8e9a14a23c54170305afb8aed38|7bf05fdc2bc11833b10823c4109152412257040f19929865cd9c2c7b52400f6a"},
    {"choose/ink_far", "5|flat ink #040810, alpha 0/255: one colour, no gradient (HD header RGB5A3)"},
    {"make_bti/ink_far/9", "RGBA8|0|0|8424d832e2812bfbe1a0265232480151ae1249469a26d16295702926f0eca7db|7bf05fdc2bc11833b10823c4109152412257040f19929865cd9c2c7b52400f6a"},
    {"make_bti/565/5", "RGB565|0|0|8a1d92b97e7bd3a13b05b12ef512c4393c44afa81c6b4756d6c2301d26b821bf|ef0c1d97c987e5a425eed8ccb74e2afcbdb1253fb2453d790148497ee9b7384b"},
    {"choose/565", "4|opaque and 565-exact"},
    {"make_bti/565/9", "RGB565|0|0|a74429e79ee077b15409e6d5d8090be149018b2ee490899b7cfd0e1321140b22|ef0c1d97c987e5a425eed8ccb74e2afcbdb1253fb2453d790148497ee9b7384b"},
    {"make_bti/rgba8/5", "RGBA8|0|0|cfd7659a6a6051efcae5f7b385ce059dc4b1fddb966f926d083fbe94b8ff30e9|b4ecfcadd69110bddabe1a5b1805f39f10a5b9d89322ab695faf2cf977b4ee75"},
    {"choose/rgba8", "6|continuous tone / soft alpha: lossless"},
    {"make_bti/rgba8/9", "RGBA8|0|0|0ac4be548f3b871b567070060a270fe42cff470366d864e6d372ed60e49f9cf6|b4ecfcadd69110bddabe1a5b1805f39f10a5b9d89322ab695faf2cf977b4ee75"},
    {"icon/0", "1b998d979f2a4abdc4b33cfc92ef83a4a62cf996f47ee1ec505ebfbf0f8a8276|e0628a1148a00b11ead4150f1099443f2b7775adc465df05c31e248d151e5908|23b5d856c6a8fbcc349c5b45b0adada34c9b23ef3d2b12a4d78a20dd82a692c4"},
    {"icon/1", "e53ed8e9d665aa4d6756a99a10f82a61ec35bb8e0da91588c61a10548f0a2dff|868b3fdcce0ffa5ee4cfd29517f0e12cd37d6867d368c81bef35205f3c72a6f1|50d54dda0937b3e1ad55a06fbff4b642e113a5e4fee9e9d7917285ff99cf4dc5"},
    {"icon/2", "ade6420a3fc25809ba4d441a27dd96296a0e7b6e0e9a9a3cd3eab68ed0024602|9837181601f8baa42fe953c1a7252f3f800762d6b197c2f4e45bfc9d9fc30ac3|31fe9c94658b196c37f5fc52f1dc57662cd9a7264ab1923a1c0683560cfb93e0"},
    {"patch_header/0", "059e0040002017730000000000000000003fdeea016a4300017c8d4a00000060"},
    {"patch_header/1", "0940002000104c512b81ecdf52c548490085a3cb01dd3a0001c510f600000061"},
    {"patch_header/2", "0ee300100008822f000000000000000001cc67ac04503110030f93a300000062"},
    {"patch_header/3", "008600080004b70d000000000000000001122c8d04c327180458164f00000063"},
    {"convert/0", "14|128|64|3|c0e343d4b6e075bf7bdd470501855f5cb47c3b45cf688cd48eb0aa038f388d0b|9be0ee89661c65dc127f74d2804cc8afb5417bd69d3e7f49e385c37da845bb92"},
    {"convert_bti/0", "eaa1ff682d4f333c1309f71e3e4d4390401883966c212a0bd3a85b837ae2594f"},
    {"bti_job/0", "a6aa2a6958e3b208e99197a9b4b1699a987e1a880c9f267bf818a1d8fc3a5c26|a857db66256a38edce6cd1374fed0c6f418312d9845dbeb57c72125965188617"},
    {"convert/1", "6|32|32|2|78046427653c8a932fc2df8d246ecd56ba19616843f23654a3721f63735cc19f|36d08deb539f7a19c520eb66665221fc7d6a454eac4947d5fd8ab548f791a222"},
    {"convert_bti/1", "f63ea97183cf02cb0697e8956bc48d1ba87a7a6b3238ae83bc1930463674079b"},
    {"bti_job/1", "03443f67b63b84421f3101a30d2a661a221a81cdfd204465c2dd28931635ee67|539aff9a9602ffa730a6ad7e2407d822b849409f91839068461931aa29cdff02"},
    {"convert/2", "1|100|60|3|613ea6403833fcfbead37990abf6a43a1ba8732dead6fa0636352d03d29be333|4c17f53d7b07c72648c428d8b5dd5caf0960c7b2dc55edd17c5eb27be6e5c9ee"},
    {"convert_bti/2", "8920472ea66956b1cb456a3e4d9c2dae3088308b224919c23948004c3176d053"},
    {"bti_job/2", "e4e35e3c699d69acc29fb3149409e4d9e9a6884eda0ef5e4d0737152e4b34906|85907d8ba7b435c3ec7b0f9de74a701a604606b930786c43e1f95e8345f0a442"},
    {"convert/3", "5|50|30|2|42200c23a1a21667acd94f2e4cdaa0db556f2cb070b8ab84993916666a2ee5b9|764314066c0994b82a332f5a1a36a96c219b2605108e7488311b881cf1bda1fa"},
    {"convert_bti/3", "665ec49010e0b270621b0c4767a88d40f5ce23005f6fdeb0f0843a509359b769"},
    {"bti_job/3", "92aed9dc7011336c8b50b5f12cfe8d6073b700724e8a889912c3ea85ee3cf757|98206543b6e6141f971fe8b3a7bbe4133da9d3d226a403657a49d66f3184f9bb"},
    {"convert/4", "2|50|30|2|8ba00efac2e7d190e6ec33c291781b196c99cc49918ccc71fd1a5f5da484671e|4d20af3c06f30eb6af67a17cc55787e90ebcdbd3f3b6ffee2d15db6afab42217"},
    {"convert_bti/4", "372bdf9d3395bb6b5fa0ba6ef71be7d980ba6bcd650034fe1db6946d477df505"},
    {"bti_job/4", "adba5ffb0b1923a841236bc0d6423b79e605d87851166ab0ef11c1da4b144537|e4ea033c146775c715352d38db0dc8a25e98476fcf2deef09e800803ca46b8f5"},
    {"convert/5", "5|40|40|1|4e68b3275bb64d89954535170d2ccaa3a4d83083a0b13cb0588ad4bad01d12a0|81b3977ac9d988e2e8e38b9fdfbdd71177c9447fa0cd925756981d0624f7dff7"},
    {"convert_bti/5", "cb2461830939029c14d4f44c199f45047026e6622b2a8b715611d5e1fd85e019"},
    {"bti_job/5", "8c871cb93ff4d75a9dea67efbc0fb5e3446ca43165cad4f2a33c43e5c293c4a1|26a6e4961005652be2a2d251f75edadbc632a618da3d2595a3d6981569da65ab"},
    {"convert/6", "5|32|16|2|5016693878991888590ebd1e5a56d752ac8544387ad4b85661896a99dd131097|07b0b1fef9e6afe46d456c9f7d87886ef76e57d77b63df5b8f210a36f383c60b"},
    {"convert_bti/6", "88d380a21672240f5d6548028a24c6f973ca29fcff3abe91065c640df663ff3f"},
    {"bti_job/6", "1a19396270e8bd7f42294996848fb4e6578da91e00bbfc9229aef896ec21ef1f|d5cca7ae2090660be7832cf40c7b86db8af2f7b270cd3d263e29e4cf93d73b3b"},
};

std::string want(std::string_view name) {
    for (const Vec& v : kVectors)
        if (name == v.name) return v.value;
    return "<no vector " + std::string(name) + ">";
}

// golden_tex.Rng
struct Rng {
    u32 x;
    explicit Rng(u64 seed) : x(static_cast<u32>(seed & 0x7fffffff)) {}
    u8 byte() {
        x = (x * 1103515245u + 12345u) & 0x7fffffffu;
        return static_cast<u8>((x >> 16) & 255);
    }
    Bytes bytes(std::size_t n) {
        Bytes b(n);
        for (auto& v : b) v = byte();
        return b;
    }
};

u32 ru(u32 v, u32 a) { return (v + a - 1) / a * a; }
std::string sha(View d) { return hdf::sha256_hex(d); }
void cat(Bytes& acc, const Bytes& b) { acc.insert(acc.end(), b.begin(), b.end()); }

Bytes greyish(Bytes b) {
    for (std::size_t i = 0; i < b.size() / 4; i += 2) b[4 * i + 1] = b[4 * i + 2] = b[4 * i];
    return b;
}

const u32 kFmts[] = {0x01, 0x02, 0x07, 0x08, 0x1A, 0x31};

struct Img {
    u32 w, h;
    Bytes rgba;
};
std::vector<Img> imgs() {
    return {{13, 9, greyish(Rng(11).bytes(13 * 9 * 4))}, {16, 8, greyish(Rng(12).bytes(16 * 8 * 4))}};
}

std::string hexs(View d) {
    static constexpr char k[] = "0123456789abcdef";
    std::string s;
    for (u8 b : d) {
        s += k[b >> 4];
        s += k[b & 15];
    }
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------- untile
HDF_TEST(tex_deswizzle_vectors_and_tphd) {
    for (u32 fmt : kFmts) {
        const u32 bpp = tex::gx2_bpp(fmt);
        const bool bc = tex::gx2_is_bcn(fmt);
        for (u32 tm : {1u, 2u, 4u, 5u, 6u}) {
            Bytes acc;
            const u32 sizes[3][2] = {{37, 21}, {64, 64}, {130, 70}};
            for (const auto& wh : sizes) {
                const u32 w = wh[0], h = wh[1];
                std::vector<u32> swzs = tm >= 4 ? std::vector<u32>{0, 0x100, 0x600, 0x700} : std::vector<u32>{0};
                for (u32 swz : swzs) {
                    const u32 ew = bc ? (w + 3) / 4 : w, eh = bc ? (h + 3) / 4 : h;
                    const u32 pitch = tm >= 4 ? ru(ew, std::max(32u, 32 * (256 / bpp / 8)))
                                      : tm == 2 ? ru(ew, std::max(8u, 256 / bpp))
                                                : ru(ew, 16);
                    Rng rng(u64{fmt} * 1000 + w * 7 + h * 13 + tm * 31 + swz);
                    const Bytes data = rng.bytes(std::size_t{pitch} * ru(eh, 64) * (bpp / 8) + 4096);
                    auto lin = tex::deswizzle(fmt, w, h, pitch, tm, swz, data);
                    HDF_CHECK(lin.has_value());
                    if (!lin) return;
                    HDF_CHECK_EQ(lin->size(), std::size_t{ew} * eh * bpp / 8);
                    const auto ref = ar::deswizzle(ar::SurfaceDesc{w, h, pitch, bpp, static_cast<ar::TileMode>(tm), swz, bc, false},
                                                   data);
                    HDF_CHECK(ref == *lin);
                    cat(acc, *lin);
                }
            }
            char name[32];
            std::snprintf(name, sizeof name, "deswizzle/%x/%u", fmt, tm);
            HDF_CHECK_EQ(sha(acc), want(name));
        }
    }
}

HDF_TEST(tex_untile_random_vs_tphd) {
    // random geometry, every supported format, linear/1D/2D thin modes, pipe and bank swizzles
    Rng r(99);
    for (int k = 0; k < 400; ++k) {
        const u32 fmt = kFmts[r.byte() % 6];
        const u32 tms[] = {1, 2, 4, 5, 6};
        const u32 tm = tms[r.byte() % 5];
        const u32 w = 1 + r.byte() % 160, h = 1 + r.byte() % 90;
        const u32 swz = (u32{r.byte()} & 7u) << 8;
        const u32 bpp = tex::gx2_bpp(fmt);
        const bool bc = tex::gx2_is_bcn(fmt);
        const u32 ew = bc ? (w + 3) / 4 : w, eh = bc ? (h + 3) / 4 : h;
        const u32 pitch = tm >= 4 ? ru(ew, std::max(32u, 32 * (256 / bpp / 8))) : ru(ew, std::max(8u, 256 / bpp));
        const Bytes data = r.bytes(std::size_t{pitch} * ru(eh, 64) * (bpp / 8) + 4096);
        auto lin = tex::deswizzle(fmt, w, h, pitch, tm, swz, data);
        HDF_CHECK(lin.has_value());
        if (!lin) return;
        const auto ref = ar::deswizzle(ar::SurfaceDesc{w, h, pitch, bpp, static_cast<ar::TileMode>(tm), swz, bc, false}, data);
        HDF_CHECK(ref == *lin);
    }
}

HDF_TEST(tex_level_info_vs_tphd_pow2) {
    // gtx2img.level_info's mip rules against tphd computeSurfaceInfo on power-of-two sizes. The GX2 2D -> 1D switch
    // level (swizzle bits 16-23) is set where AddrLib demotes, as the real TP HD files have it (6,200/6,200 levels
    // agreed in `hdf-convert test tex`).
    for (u32 fmt : kFmts) {
        const u32 bpp = tex::gx2_bpp(fmt);
        const bool bc = tex::gx2_is_bcn(fmt);
        for (u32 lw = 2; lw <= 11; ++lw)
            for (u32 lh = 2; lh <= 11; ++lh)
                for (u32 tm : {2u, 4u}) {
                    tex::GtxSurface s;
                    s.format = fmt;
                    s.width = 1u << lw;
                    s.height = 1u << lh;
                    s.mips = std::min(13u, 1 + std::max(lw, lh));
                    s.tile_mode = tm;
                    s.has_image = s.has_mip = true;
                    u32 sw = s.mips;
                    std::vector<ar::SurfaceInfoOut> outs(s.mips);
                    for (u32 L = 1; L < s.mips; ++L) {
                        ar::computeSurfaceInfo(ar::SurfaceInfoIn{s.width, s.height, bpp, L, static_cast<ar::TileMode>(tm), bc},
                                               outs[L]);
                        if (tm == 4 && sw == s.mips && static_cast<u32>(outs[L].tileMode) != 4) sw = L;
                    }
                    s.swizzle = sw << 16;
                    for (u32 L = 1; L < s.mips; ++L) {
                        auto li = tex::level_info(s, static_cast<int>(L));
                        HDF_CHECK(li.has_value());
                        if (!li) return;
                        HDF_CHECK_EQ(li->pitch, outs[L].pitch);
                        HDF_CHECK_EQ(li->tile_mode, static_cast<u32>(outs[L].tileMode));
                    }
                }
    }
}

HDF_TEST(tex_to_rgba_vectors) {
    const char* comps[] = {"00010203", "00000001", "00000000", "00010205", "05040302"};
    for (u32 fmt : kFmts) {
        Bytes acc;
        for (const char* comp : comps) {
            tex::GtxSurface s;
            s.format = fmt;
            s.width = 13;
            s.height = 9;
            const u32 cv = static_cast<u32>(std::strtoul(comp, nullptr, 16));
            for (int c = 0; c < 4; ++c) s.comp_sel[static_cast<std::size_t>(c)] = static_cast<u8>(cv >> (24 - 8 * c));
            s.has_comp_sel = true;
            const std::size_t n = fmt == 0x31 ? 4 * 3 * 8 : 13 * 9 * tex::gx2_bpp(fmt) / 8;
            const Bytes lin = Rng(fmt + cv).bytes(n);
            auto rgba = tex::to_rgba(s, 0, lin);
            HDF_CHECK(rgba.has_value());
            if (!rgba) return;
            cat(acc, *rgba);
        }
        char name[32];
        std::snprintf(name, sizeof name, "to_rgba/%x", fmt);
        HDF_CHECK_EQ(sha(acc), want(name));
    }
}

// ---------------------------------------------------------------------------------------------- GX codecs
HDF_TEST(tex_gx_model_vectors) {
    const auto im = imgs();
    for (u8 f = 0; f <= 6; ++f) {
        Bytes enc_all, dec_all;
        for (const Img& i : im) {
            const Bytes e = tex::encode_model(static_cast<tex::Gx>(f), i.rgba, i.w, i.h);
            HDF_CHECK(!e.empty());
            cat(enc_all, e);
            cat(dec_all, tex::decode_model(static_cast<tex::Gx>(f), e, i.w, i.h));
        }
        HDF_CHECK_EQ(sha(enc_all), want("gx_encode/" + std::to_string(f)));
        HDF_CHECK_EQ(sha(dec_all), want("gx_decode/" + std::to_string(f)));
    }
    Bytes cm = tex::decode_model(tex::Gx::CMPR, Rng(13).bytes(128), 16, 16);
    cat(cm, tex::decode_model(tex::Gx::CMPR, Rng(14).bytes(128), 12, 10));
    HDF_CHECK_EQ(sha(cm), want("gx_decode/14"));
    Bytes bc;
    const u32 bs[3][2] = {{13, 9}, {16, 16}, {20, 4}};
    for (const auto& wh : bs) {
        const u32 w = wh[0], h = wh[1];
        cat(bc, tex::bc1_to_cmpr(Rng(15 + w).bytes(std::size_t{(w + 3) / 4} * ((h + 3) / 4) * 8), w, h));
    }
    HDF_CHECK_EQ(sha(bc), want("bc1_to_cmpr"));
    Bytes bx = tex::box_downscale(Rng(16).bytes(16 * 12 * 4), 16, 12, 8, 6);
    cat(bx, tex::box_downscale(Rng(17).bytes(10 * 10 * 4), 10, 10, 3, 3));
    HDF_CHECK_EQ(sha(bx), want("box_downscale"));
    HDF_CHECK(tex::encode_model(tex::Gx::CMPR, im[0].rgba, im[0].w, im[0].h).empty());  // Python raises
}

HDF_TEST(tex_gx_ui_vectors) {
    const auto im = imgs();
    for (u8 f = 0; f <= 6; ++f) {
        Bytes enc_all, dec_all;
        for (const Img& i : im) {
            const Bytes e = tex::encode_ui(static_cast<tex::Gx>(f), i.rgba, i.w, i.h);
            HDF_CHECK_EQ(e.size(), tex::tex_bytes(static_cast<tex::Gx>(f), i.w, i.h));
            cat(enc_all, e);
            cat(dec_all, tex::decode_ui(static_cast<tex::Gx>(f), e, i.w, i.h));
        }
        HDF_CHECK_EQ(sha(enc_all), want("ui_encode/" + std::to_string(f)));
        HDF_CHECK_EQ(sha(dec_all), want("ui_decode/" + std::to_string(f)));
    }
}

HDF_TEST(tex_ui_lossless_round_trips) {
    Rng r(5);
    const u32 w = 21, h = 11;
    const std::size_t n = std::size_t{w} * h;
    Bytes any = r.bytes(n * 4), grey(n * 4), grey17(n * 4), c565(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        const u8 v = r.byte(), a = r.byte();
        grey[4 * i] = grey[4 * i + 1] = grey[4 * i + 2] = v;
        grey[4 * i + 3] = a;
        grey17[4 * i] = grey17[4 * i + 1] = grey17[4 * i + 2] = static_cast<u8>((v % 16) * 17);
        grey17[4 * i + 3] = static_cast<u8>((a % 16) * 17);
        const u32 q5 = r.byte() % 32, q6 = r.byte() % 64, q5b = r.byte() % 32;
        c565[4 * i] = static_cast<u8>((q5 << 3) | (q5 >> 2));
        c565[4 * i + 1] = static_cast<u8>((q6 << 2) | (q6 >> 4));
        c565[4 * i + 2] = static_cast<u8>((q5b << 3) | (q5b >> 2));
        c565[4 * i + 3] = 255;
    }
    auto rt = [&](tex::Gx f, const Bytes& src) { return tex::decode_ui(f, tex::encode_ui(f, src, w, h), w, h); };
    HDF_CHECK(rt(tex::Gx::RGBA8, any) == any);
    HDF_CHECK(rt(tex::Gx::IA8, grey) == grey);
    HDF_CHECK(rt(tex::Gx::IA4, grey17) == grey17);
    HDF_CHECK(rt(tex::Gx::RGB565, c565) == c565);
    HDF_CHECK(tex::choose_ui_format(grey17, 5).format == tex::Gx::IA4);
    HDF_CHECK(tex::choose_ui_format(c565, 5).format == tex::Gx::RGB565);
    HDF_CHECK(tex::choose_ui_format(any, 5).format == tex::Gx::RGBA8);
    // the model decoder inverts the model encoder exactly where both are lossless
    HDF_CHECK(tex::decode_model(tex::Gx::RGBA8, tex::encode_model(tex::Gx::RGBA8, any, w, h), w, h) == any);
}

HDF_TEST(tex_make_bti_vectors) {
    const std::size_t n = 120;
    const Bytes rnd = Rng(21).bytes(n * 4);
    auto img = [&](auto fn) {
        Bytes b(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            const std::array<u8, 4> p = fn(i);
            std::memcpy(b.data() + 4 * i, p.data(), 4);
        }
        return b;
    };
    auto e5 = [](u32 q) { return static_cast<u8>((q << 3) | (q >> 2)); };
    auto e6 = [](u32 q) { return static_cast<u8>((q << 2) | (q >> 4)); };
    using P = std::array<u8, 4>;
    const std::vector<std::pair<std::string, Bytes>> cases = {
        {"i4", img([](std::size_t i) { const u8 v = static_cast<u8>((i % 16) * 17); return P{v, v, v, v}; })},
        {"i8", img([&](std::size_t i) { const u8 v = rnd[4 * i]; return P{v, v, v, v}; })},
        {"ia4", img([](std::size_t i) {
             const u8 v = static_cast<u8>((i % 16) * 17);
             return P{v, v, v, static_cast<u8>(((i * 7) % 16) * 17)};
         })},
        {"ia8", img([&](std::size_t i) { const u8 v = rnd[4 * i]; return P{v, v, v, rnd[4 * i + 1]}; })},
        {"ink", img([&](std::size_t i) { return i % 3 ? P{0x30, 0x80, 0xC8, 255} : P{rnd[4 * i], 1, 2, 0}; })},
        {"ink_ok", img([&](std::size_t i) { return i % 3 ? P{0x31, 0x84, 0xC6, 255} : P{rnd[4 * i], rnd[4 * i + 1], 2, 0}; })},
        {"ink_far", img([](std::size_t i) { return i % 2 ? P{4, 8, 16, 255} : P{0, 0, 0, 0}; })},
        {"565", img([&](std::size_t i) {
             return P{e5(static_cast<u32>(i % 32)), e6(static_cast<u32>((i * 3) % 64)), e5(static_cast<u32>((i * 5) % 32)), 255};
         })},
        {"rgba8", rnd},
    };
    static const char* names[] = {"I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8"};
    for (std::size_t k = 0; k < cases.size(); ++k) {
        for (u8 hd_fmt : {u8{5}, u8{9}}) {
            Bytes stub = Rng(30 + k).bytes(0x20);
            stub[0] = hd_fmt;
            stub[0x14] = 5;
            tex::BtiJob t;
            std::copy(stub.begin(), stub.end(), t.stub.begin());
            t.hdr = *tex::bti_fields(stub, 0);
            t.rgba = cases[k].second;
            t.w = 12;
            t.h = 10;
            auto m = tex::make_bti(t);
            HDF_CHECK(m.has_value());
            if (!m) return;
            const std::string got = std::string(names[static_cast<u8>(m->format)]) + "|" + std::to_string(m->error.visible) +
                                    "|" + std::to_string(m->error.transparent) + "|" + sha(m->bti) + "|" + sha(m->decoded);
            HDF_CHECK_EQ(got, want("make_bti/" + cases[k].first + "/" + std::to_string(hd_fmt)));
            if (hd_fmt == 5) {
                const auto c = tex::choose_ui_format(cases[k].second, hd_fmt);
                HDF_CHECK_EQ(std::to_string(static_cast<int>(c.format)) + "|" + c.why, want("choose/" + cases[k].first));
            }
        }
    }
    tex::BtiJob bad;
    bad.w = 4;
    bad.h = 4;
    bad.rgba = Bytes(10);
    HDF_CHECK(!tex::make_bti(bad).has_value());
}

// ---------------------------------------------------------------------------------------------- icons, headers
HDF_TEST(tex_icon_vectors) {
    const u32 sizes[3][2] = {{100, 100}, {37, 50}, {200, 120}};
    for (u32 k = 0; k < 3; ++k) {
        const u32 w = sizes[k][0], h = sizes[k][1];
        Bytes rgba = Rng(40 + k).bytes(std::size_t{w} * h * 4);
        for (std::size_t i = 0; i < std::size_t{w} * h; ++i) {
            const u8 opts[3] = {0, 255, rgba[4 * i + 3]};
            rgba[4 * i + 3] = opts[i % 3];
        }
        const Bytes icon = tex::make_icon_rgba(rgba, w, h);
        const Bytes bc1 = tex::bc1_encode(icon, 48, 48);
        const Bytes bti = tex::bti_cmpr(icon, 48, 48);
        HDF_CHECK_EQ(sha(icon) + "|" + sha(bc1) + "|" + sha(bti), want("icon/" + std::to_string(k)));
        HDF_CHECK(tex::stamp_icon_bti(rgba, w, h) == bti);
        HDF_CHECK(bti.size() <= 0xC00);  // build_icons' assert
    }
}

HDF_TEST(tex_patch_header_vectors) {
    const u32 cases[4][2] = {{5, 1}, {9, 1}, {14, 3}, {0, 4}};
    for (u32 k = 0; k < 4; ++k) {
        Bytes raw = Rng(50 + k).bytes(0x20);
        raw[0x14] = 4;
        const Bytes out = tex::patch_header(raw, static_cast<u8>(cases[k][0]), 64u >> k, 32u >> k, cases[k][1], 0x60 + k);
        HDF_CHECK_EQ(hexs(out), want("patch_header/" + std::to_string(k)));
    }
    HDF_CHECK_EQ(tex::gx_tex_size(14, 64, 64, 3), u64{2048 + 512 + 128});
    HDF_CHECK_EQ(tex::gx_tex_size(6, 5, 3, 1), u64{64 * 2});
}

// ---------------------------------------------------------------------------------------------- GTX end to end
namespace {

// golden_tex.make_gtx
Bytes make_gtx(Rng& rng, u32 fmt, u32 w, u32 h, u32 mips, u32 tm, u32 swizzle, u32 comp) {
    const u32 bpp = tex::gx2_bpp(fmt);
    const bool bc = tex::gx2_is_bcn(fmt);
    const u32 ew = bc ? (w + 3) / 4 : w, eh = bc ? (h + 3) / 4 : h;
    const u32 pitch = tm >= 4 ? ru(ew, std::max(32u, 32 * (256 / bpp / 8))) : ru(ew, std::max(8u, 256 / bpp));
    const u32 img_size = pitch * ru(eh, 32) * bpp / 8;
    tex::GtxSurface s;
    s.format = fmt;
    s.width = w;
    s.height = h;
    s.mips = mips;
    s.tile_mode = tm;
    s.swizzle = swizzle;
    s.pitch = pitch;
    s.has_image = s.has_mip = true;
    std::array<u32, 13> offs{};
    u32 total = 0;
    for (u32 L = 1; L < mips; ++L) {
        if (L >= 2) offs[L - 1] = total;
        total += static_cast<u32>(tex::level_info(s, static_cast<int>(L))->size);
    }
    Bytes surf(0x9C, 0);
    const u32 v[16] = {1, w, h, 1, mips, fmt, 0, 1, img_size, 0, total, 0, tm, swizzle, 0x200, pitch};
    for (int k = 0; k < 16; ++k) io::put32(surf, 4u * k, v[k]);
    for (int k = 0; k < 13; ++k) io::put32(surf, 0x40 + 4u * k, offs[static_cast<std::size_t>(k)]);
    io::put32(surf, 0x84, comp);
    auto blk = [](Bytes& out, u32 t, const Bytes& data) {
        const std::size_t o = out.size();
        out.resize(o + 0x20, 0);
        out[o] = 'B';
        out[o + 1] = 'L';
        out[o + 2] = 'K';
        out[o + 3] = '{';
        const u32 f[7] = {0x20, 1, 0, t, static_cast<u32>(data.size()), 0, 0};
        for (int k = 0; k < 7; ++k) io::put32(out, o + 4 + 4u * k, f[k]);
        out.insert(out.end(), data.begin(), data.end());
    };
    Bytes out(0x20, 0);
    out[0] = 'G';
    out[1] = 'f';
    out[2] = 'x';
    out[3] = '2';
    const u32 hdr[5] = {0x20, 7, 1, 2, 0};
    for (int k = 0; k < 5; ++k) io::put32(out, 4 + 4u * k, hdr[k]);
    blk(out, 0x0B, surf);
    blk(out, 0x0C, rng.bytes(img_size));
    if (mips > 1) blk(out, 0x0D, rng.bytes(total));
    blk(out, 0x01, {});
    return out;
}

struct GCase {
    u32 fmt, w, h, mips, tm, swz, comp;
    u8 hf;
    u16 hw, hh;
    u8 hm;
};
const GCase kGCases[] = {{0x31, 256, 128, 5, 4, 0x00030000, 0x00010203, 14, 128, 64, 3},
                         {0x1A, 64, 64, 4, 4, 0x00020000, 0x00010203, 6, 32, 32, 2},
                         {0x01, 100, 60, 3, 4, 0x00010000, 0x00000000, 1, 100, 60, 3},
                         {0x01, 100, 60, 3, 4, 0x00010000, 0x00000000, 14, 50, 30, 2},
                         {0x02, 50, 30, 2, 2, 0x00000000, 0x00000001, 2, 50, 30, 2},
                         {0x07, 40, 40, 1, 4, 0x00000000, 0x00000001, 9, 40, 40, 1},
                         {0x08, 32, 16, 2, 4, 0x00010000, 0x00010205, 5, 32, 16, 4}};

}  // namespace

HDF_TEST(tex_convert_texture_vectors) {
    for (u32 k = 0; k < std::size(kGCases); ++k) {
        const GCase& c = kGCases[k];
        Rng rng(60 + k);
        const Bytes g = make_gtx(rng, c.fmt, c.w, c.h, c.mips, c.tm, c.swz, c.comp);
        auto surfs = tex::gtx_surfaces(g);
        HDF_CHECK(surfs.has_value() && surfs->size() == 1);
        if (!surfs || surfs->empty()) return;
        tex::BtiFields hdr;
        hdr.format = c.hf;
        hdr.width = c.hw;
        hdr.height = c.hh;
        hdr.mip_count = c.hm;
        auto cv = tex::convert_texture(hdr, (*surfs)[0], 1);
        HDF_CHECK(cv.has_value());
        if (!cv) return;
        const std::string got = std::to_string(cv->format) + "|" + std::to_string(cv->width) + "|" + std::to_string(cv->height) +
                                "|" + std::to_string(cv->levels) + "|" + sha(g) + "|" + sha(cv->pixels);
        HDF_CHECK_EQ(got, want("convert/" + std::to_string(k)));
        Bytes bti = Rng(70 + k).bytes(0x20);
        bti[0] = c.hf;
        bti[0x18] = c.hm;
        io::put16(bti, 2, c.hw);
        io::put16(bti, 4, c.hh);
        io::put32(bti, 0x1C, 0);
        auto cb = tex::convert_bti(bti, View(g));
        HDF_CHECK(cb.has_value());
        if (cb) HDF_CHECK_EQ(sha(*cb), want("convert_bti/" + std::to_string(k)));
        auto job = tex::bti_job(bti, g);
        HDF_CHECK(job.has_value());
        if (!job) return;
        auto made = tex::make_bti(*job);
        HDF_CHECK(made.has_value());
        if (made) HDF_CHECK_EQ(sha(job->rgba) + "|" + sha(made->bti), want("bti_job/" + std::to_string(k)));
    }
    // a BTI that already has pixels, or no GTX: unchanged
    Bytes full(0x20 + 32, 7);
    full[0] = 6;
    io::put16(full, 2, 4);
    io::put16(full, 4, 4);
    full[0x18] = 1;
    io::put32(full, 0x1C, 0x20);
    auto same = tex::convert_bti(full, std::nullopt);
    HDF_CHECK(same.has_value() && *same == full);
}

// ---------------------------------------------------------------------------------------------- robustness
HDF_TEST(tex_parsers_survive_bad_input) {
    Rng rng(61);
    const Bytes g = make_gtx(rng, 0x31, 64, 64, 4, 4, 0x00020000, 0x00010203);
    Rng r(7);
    u32 values = 0, errors = 0;
    for (int k = 0; k < 600; ++k) {
        Bytes b = g;
        if (k % 3 == 0) {
            b.resize(r.byte() * (g.size() / 256));  // truncated
        } else {
            for (int m = 0; m < 1 + k % 7; ++m) b[(std::size_t{r.byte()} << 8 | r.byte()) % b.size()] = r.byte();  // flipped
        }
        auto s = tex::gtx_surfaces(b);
        if (!s) {
            ++errors;
            continue;
        }
        for (const auto& surf : *s) {
            for (int L = 0; L < 6; ++L) {
                if (surf.width > 4096 || surf.height > 4096) break;  // a flipped size would only make this slow
                auto lin = tex::untile(surf, L);
                if (!lin) {
                    ++errors;
                    continue;
                }
                auto rgba = tex::to_rgba(surf, L, *lin);
                rgba ? ++values : ++errors;
                tex::BtiFields h;
                h.format = static_cast<u8>(r.byte() % 16);
                h.width = 32;
                h.height = 32;
                h.mip_count = 3;
                auto cv = tex::convert_texture(h, surf, 1);
                cv ? ++values : ++errors;
            }
        }
    }
    HDF_CHECK(values > 0);
    HDF_CHECK(errors > 0);
    HDF_CHECK(!tex::gtx_surfaces(Bytes{'G', 'f', 'x'}).has_value());
    HDF_CHECK(!tex::bti_fields(Bytes(0x1F), 0).has_value());
    HDF_CHECK(!tex::bti_job(Bytes(0x10), g).has_value());
    HDF_CHECK(!tex::convert_bti(Bytes(8), View(g)).has_value());
    HDF_CHECK(tex::bc1_to_cmpr(Bytes(7), 8, 8).empty());
    HDF_CHECK(tex::box_downscale(Bytes(16), 4, 4, 0, 1).empty());
    auto gm = tex::gmx_sections(Bytes{'G', 'M', 'X', '2', 0, 0, 0, 8, 'E', 'N', 'D', 'X', 0, 0, 0, 8});
    HDF_CHECK(gm.has_value() && gm->size() == 1 && (*gm)[0].tag == "ENDX");
}

// Wave 4 (wave2_T_verify L1): a block that is not the end block, with header size 0 and data size 0, never moves
// the walk (Python loops forever; a surface block would also grow the list). It is an error now.
HDF_TEST(tex_gtx_zero_size_block_ends) {
    auto put32 = [](Bytes& b, std::size_t o, u32 v) {
        for (int k = 0; k < 4; ++k) b[o + k] = static_cast<u8>(v >> (24 - 8 * k));
    };
    for (const u32 btype : {0x05u, 0x0Bu}) {  // another block, a surface block (major 7)
        Bytes b(0x20 + 0x80, 0);
        b[0] = 'G', b[1] = 'f', b[2] = 'x', b[3] = '2';
        put32(b, 4, 0x20);
        put32(b, 8, 7);
        put32(b, 12, 1);
        put32(b, 16, 2);
        b[0x20] = 'B', b[0x21] = 'L', b[0x22] = 'K', b[0x23] = '{';
        put32(b, 0x24, 0);       // header size
        put32(b, 0x30, btype);   // type
        put32(b, 0x34, 0);       // data size
        auto r = tex::gtx_read(b);
        HDF_CHECK(!r.has_value());
        if (!r) HDF_CHECK_EQ(r.error().code, std::string("fmt.tex"));
    }
    // the end block (type 1) with zero sizes still ends the walk normally
    Bytes e(0x40, 0);
    e[0] = 'G', e[1] = 'f', e[2] = 'x', e[3] = '2';
    put32(e, 4, 0x20);
    put32(e, 8, 7);
    e[0x20] = 'B', e[0x21] = 'L', e[0x22] = 'K', e[0x23] = '{';
    put32(e, 0x30, 1);
    auto ok = tex::gtx_read(e);
    HDF_CHECK(ok.has_value() && ok->surfaces.empty() && ok->blocks == 1);
}
