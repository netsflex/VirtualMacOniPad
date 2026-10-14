// Fallback converter for single-function PVG libraries that Apple's AIR
// downgrade pass rejects exactly as Metal delivers them. It repairs only two
// measured shapes in the module text, then runs the same Apple pass, verifiers
// and library writer as metal-library-convert:
//
//  - air.convert between signed and unsigned integers of one width. AIR 2.5
//    has no such intrinsic; the conversion is the identity on the bits, so the
//    call becomes a bitcast.
//  - air.vertexFetchFunction generated for a stage-in pipeline. Its hidden
//    vertex buffer omits air.address_space, which the pass requires. The field
//    is taken from the parameter's own pointer type and the entry is renamed to
//    vz_pvg_vertex_<input SHA256>, as the helper already does for vertex fetch.
//
// Usage: vz-air-normalize <VM27GPUCompiler.dylib> <input> <output> <evidence>
// writes <output> and <evidence>.conversion.json and exits 0, or exits 1.
//
// vz-air-normalize --address-use <VM27GPUCompiler.dylib> <library> reports
// whether the function loads a pointer from memory or makes one from an
// integer. Such a shader dereferences a GPU address the guest wrote, typically
// through an argument buffer; the iPadOS 16 host cannot place resources at
// guest addresses, so those reads fault on the iPad GPU.
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace {

const unsigned kNormalizerVersion = 1;
const size_t kLibraryLimit = 16 * 1024 * 1024;
const char kFetchName[] = "air.vertexFetchFunction";

struct Version { uint32_t major, minor, patch; };

struct Api {
    void *(*ContextCreate)();
    void (*ContextDispose)(void *);
    void *(*BufferCopy)(const char *, size_t, const char *);
    void (*BufferDispose)(void *);
    const char *(*BufferStart)(void *);
    size_t (*BufferSize)(void *);
    int (*ParseBitcode)(void *, void *, void **);
    int (*ParseIR)(void *, void *, void **, char **);
    char *(*PrintModule)(void *);
    void (*DisposeMessage)(char *);
    int (*VerifyModule)(void *, int, char **);
    void (*DisposeModule)(void *);
    bool (*Downgrade)(void *, Version);
    bool (*VerifyAIR)(void *);
    void *(*MakeShared)(void *);
    void (*DisposeShared)(void *);
    void *(*FunctionCreate)(void *, const char *);
    const char *(*FunctionName)(void *);
    Version (*FunctionAIR)(void *);
    Version (*FunctionMetal)(void *);
    void *(*LibraryCreate)(const char *);
    void (*LibraryInsert)(void *, void *);
    void *(*LibraryWrite)(void *);
};

template <typename T> bool Bind(void *image, const char *name, T &slot) {
    void *symbol = dlsym(image, name);
    if (!symbol) symbol = dlsym(RTLD_DEFAULT, name);
    if (!symbol) std::printf("missing symbol %s\n", name);
    slot = reinterpret_cast<T>(symbol);
    return symbol != nullptr;
}

bool LoadApi(const char *compiler, Api &api) {
    void *image = dlopen(compiler, RTLD_NOW | RTLD_GLOBAL);
    if (!image) { std::printf("dlopen %s failed: %s\n", compiler, dlerror()); return false; }
    return Bind(image, "LLVMContextCreate", api.ContextCreate) &&
        Bind(image, "LLVMContextDispose", api.ContextDispose) &&
        Bind(image, "LLVMCreateMemoryBufferWithMemoryRangeCopy", api.BufferCopy) &&
        Bind(image, "LLVMDisposeMemoryBuffer", api.BufferDispose) &&
        Bind(image, "LLVMGetBufferStart", api.BufferStart) &&
        Bind(image, "LLVMGetBufferSize", api.BufferSize) &&
        Bind(image, "LLVMParseBitcodeInContext2", api.ParseBitcode) &&
        Bind(image, "LLVMParseIRInContext", api.ParseIR) &&
        Bind(image, "LLVMPrintModuleToString", api.PrintModule) &&
        Bind(image, "LLVMDisposeMessage", api.DisposeMessage) &&
        Bind(image, "LLVMVerifyModule", api.VerifyModule) &&
        Bind(image, "LLVMDisposeModule", api.DisposeModule) &&
        Bind(image, "MTLDowngradeAIRModule", api.Downgrade) &&
        Bind(image, "MTLVerifyAIRModule", api.VerifyAIR) &&
        Bind(image, "LLVMExtraMakeSharedModule", api.MakeShared) &&
        Bind(image, "LLVMExtraDisposeSharedModule", api.DisposeShared) &&
        Bind(image, "MTLMetalFunctionCreate", api.FunctionCreate) &&
        Bind(image, "MTLMetalFunctionGetName", api.FunctionName) &&
        Bind(image, "MTLMetalFunctionGetAIRVersion", api.FunctionAIR) &&
        Bind(image, "MTLMetalFunctionGetMetalVersion", api.FunctionMetal) &&
        Bind(image, "MTLMetalLibCreateExecutableWithTriple", api.LibraryCreate) &&
        Bind(image, "MTLMetalLibInsertFunction", api.LibraryInsert) &&
        Bind(image, "MTLWriteMetalLibToMemoryBuffer", api.LibraryWrite);
}

bool ReadFile(const char *path, std::string &out) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat value;
    if (fd < 0) return false;
    if (fstat(fd, &value) || !S_ISREG(value.st_mode) || value.st_size < 88 ||
        static_cast<uint64_t>(value.st_size) > kLibraryLimit) { close(fd); return false; }
    out.resize(static_cast<size_t>(value.st_size));
    size_t done = 0;
    while (done < out.size()) {
        ssize_t count = read(fd, &out[done], out.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += static_cast<size_t>(count);
    }
    close(fd);
    return done == out.size();
}

bool WriteNewFile(const std::string &path, const std::string &data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t count = write(fd, data.data() + done, data.size() - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        done += static_cast<size_t>(count);
    }
    bool complete = done == data.size();
    if (close(fd)) complete = false;
    if (!complete) unlink(path.c_str());
    return complete;
}

std::string Sha256(const std::string &data) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.data(), static_cast<CC_LONG>(data.size()), digest);
    char text[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    for (unsigned i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i)
        std::snprintf(text + i * 2, 3, "%02x", digest[i]);
    return text;
}

std::string Hex(const std::string &bytes) {
    std::string text;
    char pair[3];
    for (unsigned char c : bytes) { std::snprintf(pair, sizeof(pair), "%02x", c); text += pair; }
    return text;
}

uint64_t U64(const std::string &d, size_t at) { uint64_t v; std::memcpy(&v, d.data() + at, 8); return v; }
uint32_t U32(const std::string &d, size_t at) { uint32_t v; std::memcpy(&v, d.data() + at, 4); return v; }
uint16_t U16(const std::string &d, size_t at) { uint16_t v; std::memcpy(&v, d.data() + at, 2); return v; }

struct Single { std::string name; unsigned type = 0; std::string air; };

// The measured complete version-2 library with exactly one function.
bool ReadSingleFunction(const std::string &d, Single &f) {
    if (d.size() < 88 || d.size() > kLibraryLimit || d.compare(0, 4, "MTLB") ||
        U16(d, 6) != 2 || U64(d, 16) != d.size()) return false;
    uint64_t table = U64(d, 24), tableSize = U64(d, 32);
    uint64_t bitcode = U64(d, 72), bitcodeSize = U64(d, 80);
    if (table < 88 || tableSize < 8 || table + 4 + tableSize > d.size() ||
        bitcode > d.size() || bitcodeSize > d.size() - bitcode || U32(d, table) != 1) return false;
    size_t cursor = table + 8, end = table + 4 + tableSize;
    bool ended = false, haveType = false, haveOffsets = false;
    uint64_t airOffset = 0, airSize = 0;
    bool haveSize = false;
    while (end - cursor >= 4) {
        std::string tag = d.substr(cursor, 4);
        cursor += 4;
        if (tag == "ENDT") { ended = true; break; }
        if (end - cursor < 2) return false;
        uint16_t length = U16(d, cursor);
        cursor += 2;
        if (length > end - cursor) return false;
        if (tag == "NAME") {
            std::string name = d.substr(cursor, length);
            while (!name.empty() && !name.back()) name.pop_back();
            if (name.empty() || name.find('\0') != std::string::npos) return false;
            f.name = name;
        } else if (tag == "TYPE") {
            if (length != 1 || static_cast<unsigned char>(d[cursor]) > 3) return false;
            f.type = static_cast<unsigned char>(d[cursor]);
            haveType = true;
        } else if (tag == "OFFT") {
            if (length != 24) return false;
            airOffset = U64(d, cursor + 16);
            haveOffsets = true;
        } else if (tag == "MDSZ") {
            if (length != 8) return false;
            airSize = U64(d, cursor);
            haveSize = true;
        }
        cursor += length;
    }
    if (!ended || f.name.empty() || !haveType || !haveOffsets || airOffset > bitcodeSize) return false;
    if (!haveSize) airSize = bitcodeSize - airOffset;
    if (airSize < 4 || airSize > bitcodeSize - airOffset) return false;
    f.air = d.substr(bitcode + airOffset, airSize);
    return !f.air.compare(0, 4, "\xde\xc0\x17\x0b") || !f.air.compare(0, 4, "BC\xc0\xde");
}

std::vector<std::string> Lines(const std::string &text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::string Join(const std::vector<std::string> &lines) {
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i) { if (i) text += '\n'; text += lines[i]; }
    return text;
}

size_t Count(const std::string &text, const std::string &needle) {
    size_t count = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++count;
    return count;
}

// %r = call <T> @air.convert.s.T.u.T(<T> %x)  ->  %r = bitcast <T> %x to <T>
std::string RewriteIdentityConverts(const std::string &text, unsigned &count) {
    static const std::regex call(
        R"re(^(\s*)(%[-\w.$]+) = (?:tail )?call (?:fast )?(<\d+ x i(?:8|16|32|64)>|i(?:8|16|32|64)) )re"
        R"re(@air\.convert\.([su])\.((?:v\d+)?i(?:8|16|32|64))\.([su])\.((?:v\d+)?i(?:8|16|32|64)))re"
        R"re(\((<\d+ x i(?:8|16|32|64)>|i(?:8|16|32|64)) (%[-\w.$]+)\)(?: #\d+)?\s*$)re");
    std::vector<std::string> lines = Lines(text);
    std::vector<std::string> touched;
    for (std::string &line : lines) {
        std::smatch m;
        if (!std::regex_match(line, m, call) || m[5] != m[7] || m[4] == m[6] || m[3] != m[8]) continue;
        touched.push_back("air.convert." + m[4].str() + "." + m[5].str() + "." + m[6].str() + "." + m[7].str());
        line = m[1].str() + m[2].str() + " = bitcast " + m[8].str() + " " + m[9].str() + " to " + m[3].str();
        ++count;
    }
    std::string result = Join(lines);
    // Drop each now-unreferenced declaration so no AIR 2.7 intrinsic survives.
    for (const std::string &name : touched) {
        if (Count(result, "@" + name + "(") != 1) continue;
        std::vector<std::string> kept;
        for (const std::string &line : Lines(result))
            if (!(line.compare(0, 8, "declare ") == 0 && line.find("@" + name + "(") != std::string::npos))
                kept.push_back(line);
        result = Join(kept);
    }
    return result;
}

std::vector<std::string> TopLevelSplit(const std::string &text) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string current;
    for (char c : text) {
        if (c == '(' || c == '<' || c == '[' || c == '{') ++depth;
        if (c == ')' || c == '>' || c == ']' || c == '}') --depth;
        if (c == ',' && depth == 0) { parts.push_back(current); current.clear(); continue; }
        current += c;
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

bool NormalizeVertexFetch(std::string &text, const std::string &renamed, unsigned &fixed) {
    const std::string define = "define void @" + std::string(kFetchName) + "(";
    size_t at = text.find(define);
    if (at == std::string::npos) return false;
    size_t open = at + define.size() - 1, close = open;
    for (int depth = 0; close < text.size(); ++close) {
        if (text[close] == '(') ++depth;
        if (text[close] == ')' && --depth == 0) break;
    }
    if (close >= text.size()) return false;
    std::vector<std::string> params = TopLevelSplit(text.substr(open + 1, close - open - 1));
    static const std::regex hidden(
        R"re(^(!\d+ = !\{i32 (\d+), !"air\.buffer", !"air\.location_index", i32 \d+, i32 1, !"air\.(?:read|read_write|write)", ))re"
        R"re((!"air\.arg_type_name", !"void", !"air\.arg_name", !"\.hidden"\})$)re");
    static const std::regex space(R"re(addrspace\((\d+)\))re");
    std::vector<std::string> lines = Lines(text);
    for (std::string &line : lines) {
        std::smatch m, s;
        if (!std::regex_match(line, m, hidden)) continue;
        size_t index = std::stoul(m[2].str());
        if (index >= params.size() || !std::regex_search(params[index], s, space)) return false;
        line = m[1].str() + "!\"air.address_space\", i32 " + s[1].str() + ", " + m[3].str();
        ++fixed;
    }
    text = Join(lines);
    for (size_t p = text.find("@" + std::string(kFetchName)); p != std::string::npos;
         p = text.find("@" + std::string(kFetchName), p + renamed.size() + 1))
        text.replace(p + 1, std::strlen(kFetchName), renamed);
    return true;
}

struct Versions { std::string target; std::vector<unsigned> air, language; };

std::vector<unsigned> NamedVersion(const std::string &text, const std::string &name) {
    std::smatch m;
    std::regex reference("!" + std::regex_replace(name, std::regex(R"re(\.)re"), R"re(\.)re") + R"re( = !\{!(\d+)\})re");
    if (!std::regex_search(text, m, reference)) return {};
    std::regex node("\n!" + m[1].str() + R"re( = !\{([^\n]*)\})re");
    if (!std::regex_search(text, m, node)) return {};
    std::vector<unsigned> numbers;
    std::string body = m[1].str();
    std::regex number(R"re(i32 (\d+))re");
    for (auto i = std::sregex_iterator(body.begin(), body.end(), number); i != std::sregex_iterator(); ++i)
        numbers.push_back(static_cast<unsigned>(std::stoul((*i)[1].str())));
    return numbers;
}

Versions ModuleVersions(const std::string &text) {
    Versions v;
    std::smatch m;
    if (std::regex_search(text, m, std::regex(R"re(target triple = "([^"]+)")re"))) v.target = m[1].str();
    v.air = NamedVersion(text, "air.version");
    v.language = NamedVersion(text, "air.language_version");
    return v;
}

std::string Text(Api &api, void *module) {
    char *text = api.PrintModule(module);
    std::string result = text ? text : "";
    if (text) api.DisposeMessage(text);
    return result;
}

std::string JsonString(const std::string &value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20 || c >= 0x7f) { char e[8]; std::snprintf(e, sizeof(e), "\\u%04x", c); out += e; }
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string JsonVersion(const std::vector<unsigned> &v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + std::to_string(v[i]);
    return out + "]";
}

// Counts loads whose result is a pointer, and integer-to-pointer casts.
void CountAddressUse(const std::string &text, unsigned &pointerLoads, unsigned &intToPtr) {
    static const std::regex load(R"re(= load (?:volatile )?([^,]+), )re");
    for (const std::string &line : Lines(text)) {
        std::smatch m;
        if (std::regex_search(line, m, load) && !m[1].str().empty() && m[1].str().back() == '*') ++pointerLoads;
        if (line.find(" inttoptr ") != std::string::npos) ++intToPtr;
    }
}

int AddressUse(const char *compiler, const char *path) {
    std::string input;
    Single function;
    if (!ReadFile(path, input) || !ReadSingleFunction(input, function)) {
        std::printf("input is not a single-function version-2 Metal library\n");
        return 1;
    }
    Api api;
    if (!LoadApi(compiler, api)) return 1;
    void *context = api.ContextCreate();
    void *module = nullptr;
    void *bitcode = api.BufferCopy(function.air.data(), function.air.size(), "input");
    int failed = api.ParseBitcode(context, bitcode, &module);
    api.BufferDispose(bitcode);
    if (failed || !module) { std::printf("Apple LLVM could not parse the input AIR\n"); return 1; }
    unsigned pointerLoads = 0, intToPtr = 0;
    CountAddressUse(Text(api, module), pointerLoads, intToPtr);
    std::printf("function=%s address-use=%d pointerLoads=%u intToPtr=%u\n", function.name.c_str(),
                pointerLoads || intToPtr, pointerLoads, intToPtr);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    if (argc == 4 && !std::strcmp(argv[1], "--address-use")) return AddressUse(argv[2], argv[3]);
    if (argc != 5) { std::printf("usage: %s compiler input output evidence\n", argv[0]); return 1; }
    const char *compiler = argv[1], *inputPath = argv[2], *outputPath = argv[3];
    std::string evidencePath = std::string(argv[4]) + ".conversion.json";
    std::string input;
    Single function;
    if (!ReadFile(inputPath, input) || !ReadSingleFunction(input, function)) {
        std::printf("input is not a single-function version-2 Metal library\n");
        return 1;
    }
    std::string inputHash = Sha256(input);
    bool fetch = function.name == kFetchName;
    std::string name = fetch ? "vz_pvg_vertex_" + inputHash : function.name;
    std::printf("input-function=%s type=%u bytes=%zu sha256=%s\n", function.name.c_str(),
                function.type, input.size(), inputHash.c_str());
    if (!fetch && !function.name.compare(0, 4, "air.")) {
        std::printf("private AIR function is not a measured normalization shape\n");
        return 1;
    }
    Api api;
    if (!LoadApi(compiler, api)) return 1;

    void *context = api.ContextCreate();
    void *module = nullptr;
    void *bitcode = api.BufferCopy(function.air.data(), function.air.size(), "input");
    int failed = api.ParseBitcode(context, bitcode, &module);
    api.BufferDispose(bitcode);
    if (failed || !module) { std::printf("Apple LLVM could not parse the input AIR\n"); return 1; }
    std::string before = Text(api, module);
    api.DisposeModule(module);
    module = nullptr;

    unsigned converts = 0, hidden = 0;
    std::string normalized = RewriteIdentityConverts(before, converts);
    if (fetch && !NormalizeVertexFetch(normalized, name, hidden)) {
        std::printf("vertex fetch entry or its hidden buffer pointer was not found\n");
        return 1;
    }
    if (!converts && !hidden) {
        std::printf("no measured normalization applies; leaving the input to the Apple helper\n");
        return 1;
    }
    std::printf("normalized converts=%u hiddenBuffers=%u renamed=%d\n", converts, hidden, fetch);

    void *irBuffer = api.BufferCopy(normalized.data(), normalized.size(), "normalized");
    char *message = nullptr;
    if (api.ParseIR(context, irBuffer, &module, &message) || !module) {  // takes the buffer
        std::printf("normalized IR does not parse: %s\n", message ? message : "unknown");
        return 1;
    }
    bool changed = api.Downgrade(module, Version{2, 5, 0});
    if (!api.VerifyAIR(module)) { std::printf("Apple AIR verifier rejected the module\n"); return 1; }
    message = nullptr;
    if (api.VerifyModule(module, 2, &message)) {
        std::printf("Apple LLVM verifier rejected the module: %s\n", message ? message : "unknown");
        return 1;
    }
    if (message) api.DisposeMessage(message);
    Versions after = ModuleVersions(Text(api, module));
    std::printf("after target=%s AIR=%s Metal=%s downgradeChanged=%d\n", after.target.c_str(),
                JsonVersion(after.air).c_str(), JsonVersion(after.language).c_str(), changed);
    if (after.air != std::vector<unsigned>{2, 5, 0} || after.language.size() != 3 ||
        after.language > std::vector<unsigned>{3, 0, 0}) {
        std::printf("Apple pass did not reach AIR 2.5 with Metal 3.0 or older\n");
        return 1;
    }

    void *shared = api.MakeShared(module);
    void *metalFunction = api.FunctionCreate(shared, name.c_str());
    if (!metalFunction || !api.FunctionName(metalFunction) || name != api.FunctionName(metalFunction)) {
        std::printf("Apple could not rebuild the named function\n");
        return 1;
    }
    Version air = api.FunctionAIR(metalFunction), metal = api.FunctionMetal(metalFunction);
    if (std::vector<unsigned>{air.major, air.minor, air.patch} != after.air ||
        std::vector<unsigned>{metal.major, metal.minor, metal.patch} != after.language) {
        std::printf("Apple function reflection disagrees with the module\n");
        return 1;
    }
    void *library = api.LibraryCreate(after.target.c_str());
    if (!library) { std::printf("Apple could not create a %s library\n", after.target.c_str()); return 1; }
    api.LibraryInsert(library, metalFunction);
    void *written = api.LibraryWrite(library);
    if (!written) { std::printf("Apple library writer failed\n"); return 1; }
    std::string output(api.BufferStart(written), api.BufferSize(written));
    api.BufferDispose(written);
    Single check;
    if (!ReadSingleFunction(output, check) || check.name != name || check.type != function.type) {
        std::printf("output failed its structural round trip\n");
        return 1;
    }
    std::string outputHash = Sha256(output);
    std::string evidence = std::string("{\n") +
        "  \"schema\":3,\n"
        "  \"conversionMode\":\"air-normalization\",\n"
        "  \"normalizerVersion\":" + std::to_string(kNormalizerVersion) + ",\n"
        "  \"compiler\":" + JsonString(compiler) + ",\n"
        "  \"input\":" + JsonString(inputPath) + ",\n"
        "  \"output\":" + JsonString(outputPath) + ",\n"
        "  \"inputSHA256\":\"" + inputHash + "\",\n"
        "  \"outputSHA256\":\"" + outputHash + "\",\n"
        "  \"inputBytes\":" + std::to_string(input.size()) + ",\n"
        "  \"outputBytes\":" + std::to_string(output.size()) + ",\n"
        "  \"inputFunction\":" + JsonString(function.name) + ",\n"
        "  \"inputFunctionBytesHex\":\"" + Hex(function.name) + "\",\n"
        "  \"function\":" + JsonString(name) + ",\n"
        "  \"functionBytesHex\":\"" + Hex(name) + "\",\n"
        "  \"functionType\":" + std::to_string(function.type) + ",\n"
        "  \"convertCallsRewritten\":" + std::to_string(converts) + ",\n"
        "  \"hiddenBuffersNormalized\":" + std::to_string(hidden) + ",\n"
        "  \"entryRenamed\":" + (fetch ? "true" : "false") + ",\n"
        "  \"afterTarget\":" + JsonString(after.target) + ",\n"
        "  \"afterAIR\":" + JsonVersion(after.air) + ",\n"
        "  \"afterMetal\":" + JsonVersion(after.language) + ",\n"
        "  \"appleDowngradeReturnedTrue\":" + (changed ? "true" : "false") + ",\n"
        "  \"appleAIRVerifierAfter\":true,\n"
        "  \"appleLLVMVerifierAfter\":true,\n"
        "  \"gpuWorkSubmitted\":false\n"
        "}\n";
    if (!WriteNewFile(outputPath, output) || !WriteNewFile(evidencePath, evidence)) {
        std::printf("could not write the output or evidence\n");
        return 1;
    }
    std::printf("converted=%s bytes=%zu sha256=%s\n", name.c_str(), output.size(), outputHash.c_str());
    return 0;
}
