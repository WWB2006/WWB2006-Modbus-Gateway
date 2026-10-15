#include "gateway/json/JsonValue.h"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace gateway::json {
namespace {

// 递归下降解析器。原先内嵌在 SimulatorConfig.cpp 的匿名命名空间里，
// 阶段 5 引入网关配置后两份配置都要用，因此抽到这里共用一份语法实现。
class Parser {
public:
    Parser(const std::string& text, std::string& error) : text_(text), error_(error) {}

    bool parseRoot(Value& out) {
        skipWhitespace();
        if (!parseValue(out)) {
            return false;
        }
        skipWhitespace();
        if (pos_ != text_.size()) {
            return fail("文件末尾存在多余内容");
        }
        return true;
    }

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

    bool parseValue(Value& target) {
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
            if (!parseValue(value)) {
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
            if (!parseValue(value)) {
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
                        return fail(std::string("不支持转义 \\") + escaped +
                                    "（只支持 \\\" \\\\ \\/ \\n \\t \\r）");
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
            if (std::isdigit(static_cast<unsigned char>(ch)) != 0 || ch == '.' || ch == 'e' ||
                ch == 'E' || ch == '+' || ch == '-') {
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
};

}  // namespace

bool parse(const std::string& text, Value& out, std::string& error) {
    error.clear();
    if (text.empty()) {
        error = "配置内容为空";
        return false;
    }
    Parser parser(text, error);
    return parser.parseRoot(out);
}

}  // namespace gateway::json
