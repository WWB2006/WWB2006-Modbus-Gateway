#include "gateway/sim/SimulatorConfig.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace gateway::sim {
namespace {

// ---------------------------------------------------------------------------
// 极简 JSON 解析器。
//
// 只支持本配置文件需要的子集：对象、数组、字符串、数字、true/false/null。
// 不做转义序列的完整处理（\u 等）—— 配置文件里不会出现，
// 与其写半套转义逻辑不如明确不支持，避免「看起来支持实际有坑」。
//
// 为什么自研而不用第三方库：项目的核心卖点是「没装 Qt 也能构建」，
// 引入 nlohmann/json 之类的头文件库会把这条卖点变成「再下一个头文件也能构建」，
// 而这个配置文件的结构简单到不值得为此付出依赖成本。
// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(const std::string& text, std::string& error) : text_(text), error_(error) {}

    bool parse() {
        skipWhitespace();
        if (!parseValue()) {
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            return fail("文件末尾存在多余内容");
        }
        return true;
    }

    // 解析后的根节点以变体形式暴露给调用方。
    struct Value {
        enum class Type { Null, Bool, Number, String, Array, Object };
        Type type = Type::Null;
        bool boolean = false;
        double number = 0;
        std::string text;
        std::vector<Value> array;
        std::vector<std::pair<std::string, Value>> object;

        const Value* member(const std::string& key) const {
            for (const auto& entry : object) {
                if (entry.first == key) {
                    return &entry.second;
                }
            }
            return nullptr;
        }
    };

    const Value& root() const { return root_; }

private:
    bool fail(const std::string& why) {
        std::ostringstream out;
        out << "第 " << line_ << " 行第 " << column_ << " 列：" << why;
        error_ = out.str();
        return false;
    }

    void advance() {
        if (pos_ < text_.size()) {
            if (text_[pos_] == '\n') {
                ++line_;
                column_ = 1;
            } else {
                ++column_;
            }
            ++pos_;
        }
    }

    void skipWhitespace() {
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                advance();
            } else {
                break;
            }
        }
    }

    bool parseValue() {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            return fail("期待一个值，但已到文件末尾");
        }
        const char ch = text_[pos_];
        if (ch == '{') {
            return parseObject(root_);
        }
        if (ch == '[') {
            return parseArray(root_);
        }
        if (ch == '"') {
            root_.type = Value::Type::String;
            return parseString(root_.text);
        }
        if (ch == 't' || ch == 'f') {
            return parseBool(root_);
        }
        if (ch == 'n') {
            return parseNull(root_);
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            return parseNumber(root_);
        }
        return fail(std::string("无法识别的字符 '") + ch + "'");
    }

    bool parseObject(Value& target) {
        target.type = Value::Type::Object;
        advance();  // 吃掉 '{'
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            advance();
            return true;
        }
        for (;;) {
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != '"') {
                return fail("对象的键必须是字符串");
            }
            std::string key;
            if (!parseString(key)) {
                return false;
            }
            skipWhitespace();
            if (pos_ >= text_.size() || text_[pos_] != ':') {
                return fail("键之后缺少冒号");
            }
            advance();

            Value value;
            if (!parseNested(value)) {
                return false;
            }
            target.object.emplace_back(std::move(key), std::move(value));

            skipWhitespace();
            if (pos_ >= text_.size()) {
                return fail("对象没有正常结束（缺少 '}'）");
            }
            if (text_[pos_] == ',') {
                advance();
                continue;
            }
            if (text_[pos_] == '}') {
                advance();
                return true;
            }
            return fail("对象里期待 ',' 或 '}'");
        }
    }

    bool parseArray(Value& target) {
        target.type = Value::Type::Array;
        advance();  // 吃掉 '['
        skipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            advance();
            return true;
        }
        for (;;) {
            Value value;
            if (!parseNested(value)) {
                return false;
            }
            target.array.push_back(std::move(value));

            skipWhitespace();
            if (pos_ >= text_.size()) {
                return fail("数组没有正常结束（缺少 ']'）");
            }
            if (text_[pos_] == ',') {
                advance();
                continue;
            }
            if (text_[pos_] == ']') {
                advance();
                return true;
            }
            return fail("数组里期待 ',' 或 ']'");
        }
    }

    // 解析一个可以出现在键值或数组元素位置的值。
    bool parseNested(Value& target) {
        skipWhitespace();
        if (pos_ >= text_.size()) {
            return fail("期待一个值，但已到文件末尾");
        }
        const char ch = text_[pos_];
        if (ch == '{') {
            return parseObject(target);
        }
        if (ch == '[') {
            return parseArray(target);
        }
        if (ch == '"') {
            target.type = Value::Type::String;
            return parseString(target.text);
        }
        if (ch == 't' || ch == 'f') {
            return parseBool(target);
        }
        if (ch == 'n') {
            return parseNull(target);
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            return parseNumber(target);
        }
        return fail(std::string("无法识别的字符 '") + ch + "'");
    }

    bool parseString(std::string& out) {
        advance();  // 吃掉开头的引号
        out.clear();
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (ch == '"') {
                advance();
                return true;
            }
            if (ch == '\\') {
                advance();
                if (pos_ >= text_.size()) {
                    return fail("字符串里的转义没有结束");
                }
                const char escaped = text_[pos_];
                switch (escaped) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    default:
                        return fail(std::string("不支持转义 \\") + escaped + "（只支持 \\\" \\\\ \\/ \\n \\t \\r）");
                }
                advance();
                continue;
            }
            out.push_back(ch);
            advance();
        }
        return fail("字符串没有闭合的引号");
    }

    bool parseBool(Value& target) {
        if (text_.compare(pos_, 4, "true") == 0) {
            target.type = Value::Type::Bool;
            target.boolean = true;
            for (int i = 0; i < 4; ++i) {
                advance();
            }
            return true;
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            target.type = Value::Type::Bool;
            target.boolean = false;
            for (int i = 0; i < 5; ++i) {
                advance();
            }
            return true;
        }
        return fail("期待 true 或 false");
    }

    bool parseNull(Value& target) {
        if (text_.compare(pos_, 4, "null") == 0) {
            target.type = Value::Type::Null;
            for (int i = 0; i < 4; ++i) {
                advance();
            }
            return true;
        }
        return fail("期待 null");
    }

    bool parseNumber(Value& target) {
        const std::size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == '-') {
            advance();
        }
        while (pos_ < text_.size()) {
            const char ch = text_[pos_];
            if (std::isdigit(static_cast<unsigned char>(ch)) != 0 || ch == '.' ||
                ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
                advance();
            } else {
                break;
            }
        }
        const std::string token = text_.substr(start, pos_ - start);
        if (token.empty() || token == "-") {
            return fail("数字格式非法");
        }
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            return fail("数字解析失败：" + token);
        }
        target.type = Value::Type::Number;
        target.number = value;
        return true;
    }

    const std::string& text_;
    std::string& error_;
    std::size_t pos_ = 0;
    std::size_t line_ = 1;
    std::size_t column_ = 1;
    Value root_;
};

// ------------------------------------------------------------- 取值辅助

bool readSize(const Parser::Value* node, const char* key, std::size_t& out,
              std::string& error) {
    const Parser::Value* value = node != nullptr ? node->member(key) : nullptr;
    if (value == nullptr) {
        return true;  // 缺省即用默认值
    }
    if (value->type != Parser::Value::Type::Number || value->number < 0) {
        error = std::string("字段 ") + key + " 必须是非负数字";
        return false;
    }
    out = static_cast<std::size_t>(value->number);
    return true;
}

bool readU16(const Parser::Value* node, const char* key, uint16_t& out, std::string& error) {
    const Parser::Value* value = node != nullptr ? node->member(key) : nullptr;
    if (value == nullptr) {
        return true;
    }
    if (value->type != Parser::Value::Type::Number || value->number < 0 || value->number > 65535) {
        error = std::string("字段 ") + key + " 必须是 0-65535 之间的数字";
        return false;
    }
    out = static_cast<uint16_t>(value->number);
    return true;
}

bool readU8(const Parser::Value* node, const char* key, uint8_t& out, std::string& error) {
    uint16_t wide = out;
    if (!readU16(node, key, wide, error)) {
        return false;
    }
    if (wide > 255) {
        error = std::string("字段 ") + key + " 必须是 0-255 之间的数字";
        return false;
    }
    out = static_cast<uint8_t>(wide);
    return true;
}

bool readString(const Parser::Value* node, const char* key, std::string& out, std::string& error) {
    const Parser::Value* value = node != nullptr ? node->member(key) : nullptr;
    if (value == nullptr) {
        return true;
    }
    if (value->type != Parser::Value::Type::String) {
        error = std::string("字段 ") + key + " 必须是字符串";
        return false;
    }
    out = value->text;
    return true;
}

bool readBool(const Parser::Value* node, const char* key, bool& out, std::string& error) {
    const Parser::Value* value = node != nullptr ? node->member(key) : nullptr;
    if (value == nullptr) {
        return true;
    }
    if (value->type != Parser::Value::Type::Bool) {
        error = std::string("字段 ") + key + " 必须是 true 或 false";
        return false;
    }
    out = value->boolean;
    return true;
}

// 把 { "0": 100, "11": 5678 } 这样的映射读进 name -> value 的表。
template <typename T>
bool readValueMap(const Parser::Value* section, const char* key,
                  std::map<uint32_t, T>& out, std::string& error,
                  bool (*convert)(const Parser::Value&, T&, std::string&)) {
    const Parser::Value* values = section != nullptr ? section->member(key) : nullptr;
    if (values == nullptr) {
        return true;
    }
    if (values->type != Parser::Value::Type::Object) {
        error = std::string("字段 ") + key + " 必须是对象（地址 -> 值）";
        return false;
    }
    for (const auto& entry : values->object) {
        char* end = nullptr;
        const unsigned long address = std::strtoul(entry.first.c_str(), &end, 10);
        if (end == nullptr || *end != '\0') {
            error = std::string("寄存器地址不是十进制数字：") + entry.first;
            return false;
        }
        if (address > 65535u) {
            error = std::string("寄存器地址超出 0-65535：") + entry.first;
            return false;
        }
        T converted{};
        if (!convert(entry.second, converted, error)) {
            return false;
        }
        out[static_cast<uint32_t>(address)] = converted;
    }
    return true;
}

bool toU32(const Parser::Value& node, uint32_t& out, std::string& error) {
    if (node.type != Parser::Value::Type::Number || node.number < 0 || node.number > 65535) {
        error = "寄存器初值必须是 0-65535 之间的整数";
        return false;
    }
    out = static_cast<uint32_t>(node.number);
    return true;
}

bool toBool(const Parser::Value& node, bool& out, std::string& error) {
    if (node.type != Parser::Value::Type::Bool) {
        error = "位区初值必须是 true 或 false";
        return false;
    }
    out = node.boolean;
    return true;
}

}  // namespace

// ------------------------------------------------------------------- 对外接口

bool loadSimulatorConfig(const std::string& path, SimulatorConfig& out, std::string& error) {
    error.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "打不开配置文件：" + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string text = buffer.str();
    if (text.empty()) {
        error = "配置文件为空：" + path;
        return false;
    }

    Parser parser(text, error);
    if (!parser.parse()) {
        error = path + " " + error;
        return false;
    }

    const Parser::Value& root = parser.root();
    if (root.type != Parser::Value::Type::Object) {
        error = path + " 的根节点必须是对象";
        return false;
    }

    SimulatorConfig config;
    const Parser::Value* sim = root.member("simulator");
    if (sim != nullptr && sim->type != Parser::Value::Type::Object) {
        error = "simulator 必须是对象";
        return false;
    }
    if (!readString(sim, "bindAddress", config.bindAddress, error) ||
        !readU16(sim, "port", config.port, error) ||
        !readU8(sim, "unitId", config.unitId, error) ||
        !readBool(sim, "supportBroadcast", config.supportBroadcast, error) ||
        !readBool(sim, "verbose", config.verbose, error)) {
        return false;
    }

    const Parser::Value* regs = root.member("registers");
    if (regs == nullptr) {
        error = "缺少 registers 段";
        return false;
    }
    if (regs->type != Parser::Value::Type::Object) {
        error = "registers 必须是对象";
        return false;
    }

    const Parser::Value* holding = regs->member("holding");
    const Parser::Value* input = regs->member("input");
    const Parser::Value* coils = regs->member("coils");
    const Parser::Value* discrete = regs->member("discreteInputs");

    if (!readSize(holding, "size", config.holdingSize, error) ||
        !readSize(input, "size", config.inputSize, error) ||
        !readSize(coils, "size", config.coilSize, error) ||
        !readSize(discrete, "size", config.discreteInputSize, error)) {
        return false;
    }
    if (!readValueMap<uint32_t>(holding, "values", config.holdingValues, error, toU32) ||
        !readValueMap<uint32_t>(input, "values", config.inputValues, error, toU32) ||
        !readValueMap<bool>(coils, "values", config.coilValues, error, toBool) ||
        !readValueMap<bool>(discrete, "values", config.discreteInputValues, error, toBool)) {
        return false;
    }

    // 初值地址必须落在声明的区内：写在外面等于「配了但不生效」，
    // 这类静默失效比直接报错更难查，所以在这里就拦住。
    for (const auto& entry : config.holdingValues) {
        if (entry.first >= config.holdingSize) {
            error = "holding.values 里的地址 " + std::to_string(entry.first) +
                    " 超出了 size " + std::to_string(config.holdingSize);
            return false;
        }
    }
    for (const auto& entry : config.inputValues) {
        if (entry.first >= config.inputSize) {
            error = "input.values 里的地址 " + std::to_string(entry.first) +
                    " 超出了 size " + std::to_string(config.inputSize);
            return false;
        }
    }
    for (const auto& entry : config.coilValues) {
        if (entry.first >= config.coilSize) {
            error = "coils.values 里的地址 " + std::to_string(entry.first) +
                    " 超出了 size " + std::to_string(config.coilSize);
            return false;
        }
    }
    for (const auto& entry : config.discreteInputValues) {
        if (entry.first >= config.discreteInputSize) {
            error = "discreteInputs.values 里的地址 " + std::to_string(entry.first) +
                    " 超出了 size " + std::to_string(config.discreteInputSize);
            return false;
        }
    }

    out = std::move(config);
    return true;
}

void applyTo(RegisterMap& map, const SimulatorConfig& config) {
    map.resizeHoldingRegisters(config.holdingSize);
    map.resizeInputRegisters(config.inputSize);
    map.resizeCoils(config.coilSize);
    map.resizeDiscreteInputs(config.discreteInputSize);

    for (const auto& entry : config.holdingValues) {
        map.setHoldingRegister(static_cast<uint16_t>(entry.first),
                               static_cast<uint16_t>(entry.second));
    }
    for (const auto& entry : config.inputValues) {
        map.setInputRegister(static_cast<uint16_t>(entry.first),
                             static_cast<uint16_t>(entry.second));
    }
    for (const auto& entry : config.coilValues) {
        map.setCoil(static_cast<uint16_t>(entry.first), entry.second);
    }
    for (const auto& entry : config.discreteInputValues) {
        map.setDiscreteInput(static_cast<uint16_t>(entry.first), entry.second);
    }
}

}  // namespace gateway::sim
