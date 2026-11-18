#include "icelib/waveform.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <complex>
#include <limits>
#include <memory>
#include <numbers>
#include <sstream>
#include <utility>

namespace ice {
namespace {

constexpr double epsilon = 1e-30;
constexpr double decibelFloor = -200.0;

std::string trim(std::string text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

std::string lowerNoSpace(const std::string &text) {
    std::string result;
    result.reserve(text.size());
    for (const auto character : text) {
        if (!std::isspace(static_cast<unsigned char>(character)))
            result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

bool unwrapSignal(const std::string &normalized, std::string &inner, char &wrapper) {
    if (normalized.size() > 3 && (normalized.rfind("v(", 0) == 0 || normalized.rfind("i(", 0) == 0) &&
        normalized.back() == ')') {
        wrapper = normalized.front();
        inner = normalized.substr(2, normalized.size() - 3);
        return true;
    }
    return false;
}

bool sameSignalName(const std::string &left, const std::string &right) {
    const auto normalizedLeft = lowerNoSpace(left);
    const auto normalizedRight = lowerNoSpace(right);
    if (normalizedLeft == normalizedRight)
        return true;
    std::string innerLeft;
    std::string innerRight;
    char wrapperLeft = 0;
    char wrapperRight = 0;
    const bool wrappedLeft = unwrapSignal(normalizedLeft, innerLeft, wrapperLeft);
    const bool wrappedRight = unwrapSignal(normalizedRight, innerRight, wrapperRight);
    if (wrappedLeft && wrappedRight)
        return wrapperLeft == wrapperRight && innerLeft == innerRight;
    // A bare node such as `out` also names the voltage trace `v(out)`. Current
    // wrappers are never inferred because the same string could name a node.
    if (wrappedLeft && wrapperLeft == 'v')
        return normalizedRight == innerLeft;
    if (wrappedRight && wrapperRight == 'v')
        return normalizedLeft == innerRight;
    return false;
}

bool isAxisName(const std::string &name, const SimulationResult &result) {
    const auto normalized = lowerNoSpace(name);
    if (normalized.empty() || normalized == "time" || normalized == "t" || normalized == "time(s)")
        return true;
    return !result.xAxis.empty() && normalized == lowerNoSpace(result.xAxis);
}

std::vector<double> clippedValues(const SimulationTrace &trace, std::size_t count) {
    std::vector<double> values(
        trace.values.begin(), trace.values.begin() + static_cast<std::ptrdiff_t>(std::min(count, trace.values.size())));
    values.resize(count, std::numeric_limits<double>::quiet_NaN());
    return values;
}

const SimulationTrace *findSignalTrace(const SimulationResult &result, const std::string &name) {
    if (name.empty())
        return nullptr;
    for (const auto &trace : result.traces) {
        if (sameSignalName(trace.name, name))
            return &trace;
    }
    // A bare node such as `out` also matches `v(out)`.
    const auto wrapped = "v(" + name + ")";
    for (const auto &trace : result.traces) {
        if (sameSignalName(trace.name, wrapped))
            return &trace;
    }
    return nullptr;
}

const std::vector<double> *axisValues(const SimulationResult &result, const std::string &name) {
    if (!isAxisName(name, result))
        return nullptr;
    return &result.time;
}

std::vector<double> decibelValues(const std::vector<double> &values) {
    std::vector<double> result;
    result.reserve(values.size());
    for (const double value : values) {
        if (!std::isfinite(value)) {
            result.push_back(std::numeric_limits<double>::quiet_NaN());
            continue;
        }
        result.push_back(std::max(decibelFloor, 20.0 * std::log10(std::max(std::abs(value), epsilon))));
    }
    return result;
}

std::size_t nextPowerOfTwo(std::size_t value) {
    std::size_t result = 1;
    while (result < value && result < (std::size_t{1} << 20))
        result <<= 1;
    return std::max<std::size_t>(result, 2);
}

double windowCoefficient(const std::string &window, std::size_t index, std::size_t count) {
    if (count <= 1)
        return 1.0;
    const auto phase = 2.0 * std::numbers::pi * static_cast<double>(index) / static_cast<double>(count - 1);
    if (window == "hann")
        return 0.5 - 0.5 * std::cos(phase);
    if (window == "hamming")
        return 0.54 - 0.46 * std::cos(phase);
    if (window == "blackman")
        return 0.42 - 0.5 * std::cos(phase) + 0.08 * std::cos(2.0 * phase);
    return 1.0;
}

void fftInPlace(std::vector<std::complex<double>> &data) {
    const auto count = data.size();
    if (count <= 1)
        return;
    for (std::size_t index = 1, reversed = 0; index < count; ++index) {
        std::size_t bit = count >> 1;
        for (; reversed & bit; bit >>= 1)
            reversed ^= bit;
        reversed ^= bit;
        if (index < reversed)
            std::swap(data[index], data[reversed]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1) {
        const auto angle = -2.0 * std::numbers::pi / static_cast<double>(length);
        const std::complex<double> rotation(std::cos(angle), std::sin(angle));
        for (std::size_t start = 0; start < count; start += length) {
            std::complex<double> factor(1.0, 0.0);
            for (std::size_t offset = 0; offset < length / 2; ++offset) {
                const auto even = data[start + offset];
                const auto odd = data[start + offset + length / 2] * factor;
                data[start + offset] = even + odd;
                data[start + offset + length / 2] = even - odd;
                factor *= rotation;
            }
        }
    }
}

double interpolate(const std::vector<double> &x, const std::vector<double> &y, double target, std::size_t &hint) {
    const auto count = std::min(x.size(), y.size());
    if (count == 0)
        return std::numeric_limits<double>::quiet_NaN();
    if (target <= x.front())
        return y.front();
    if (target >= x.back())
        return y.back();
    hint = std::min(hint, count - 2);
    while (hint + 1 < count && x[hint + 1] < target)
        ++hint;
    while (hint > 0 && x[hint] > target)
        --hint;
    const auto span = x[hint + 1] - x[hint];
    if (std::abs(span) <= epsilon)
        return y[hint];
    const auto fraction = (target - x[hint]) / span;
    return y[hint] + (y[hint + 1] - y[hint]) * fraction;
}

// ── Expression evaluation ─────────────────────────────────────────────────

struct ExprNode;
using ExprPtr = std::unique_ptr<ExprNode>;

struct ExprNode {
    enum class Kind { constant, name, unary, binary, call };
    Kind kind = Kind::constant;
    double constant = 0.0;
    std::string text;
    char op = '+';
    std::vector<ExprPtr> args;
};

struct Token {
    enum class Kind { number, identifier, symbol, end };
    Kind kind = Kind::end;
    double number = 0.0;
    std::string text;
};

bool identifierStart(char character) {
    return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '@' ||
           character == '#' || character == '$';
}

bool identifierPart(char character) {
    return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_' || character == '.' ||
           character == '@' || character == '#' || character == '[' || character == ']' || character == '$' ||
           character == ':';
}

class ExpressionParser {
  public:
    ExpressionParser(const std::string &expression, const SimulationResult &result)
        : expression_(expression), result_(result) {
        tokenize();
    }

    ExprPtr parse(std::string &error) {
        auto node = parseAdditive();
        if (!node) {
            error = error_.empty() ? "empty expression" : error_;
            return nullptr;
        }
        if (peek().kind != Token::Kind::end) {
            error = "unexpected token '" + peek().text + "'";
            return nullptr;
        }
        return node;
    }

    bool resolve(const ExprNode &node, std::vector<double> &values, std::string &error) {
        error_.clear();
        const auto result = evaluate(node, values);
        if (!result) {
            error = error_.empty() ? "expression could not be evaluated" : error_;
            return false;
        }
        return true;
    }

  private:
    void tokenize() {
        std::size_t index = 0;
        while (index < expression_.size()) {
            const auto character = expression_[index];
            if (std::isspace(static_cast<unsigned char>(character))) {
                ++index;
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(character)) != 0 ||
                (character == '.' && index + 1 < expression_.size() &&
                 std::isdigit(static_cast<unsigned char>(expression_[index + 1])) != 0)) {
                const auto start = index;
                bool exponent = false;
                while (index < expression_.size()) {
                    const auto current = expression_[index];
                    if (std::isdigit(static_cast<unsigned char>(current)) != 0 || current == '.') {
                        ++index;
                        continue;
                    }
                    if (!exponent && (current == 'e' || current == 'E')) {
                        exponent = true;
                        ++index;
                        if (index < expression_.size() && (expression_[index] == '+' || expression_[index] == '-'))
                            ++index;
                        continue;
                    }
                    break;
                }
                Token token;
                token.kind = Token::Kind::number;
                token.text = expression_.substr(start, index - start);
                const auto parsed =
                    std::from_chars(token.text.data(), token.text.data() + token.text.size(), token.number);
                if (parsed.ec != std::errc{}) {
                    error_ = "invalid number in expression: '" + token.text + "'";
                    return;
                }
                tokens_.push_back(std::move(token));
                continue;
            }
            if (identifierStart(character)) {
                const auto start = index;
                while (index < expression_.size() && identifierPart(expression_[index]))
                    ++index;
                Token token;
                token.kind = Token::Kind::identifier;
                token.text = expression_.substr(start, index - start);
                tokens_.push_back(std::move(token));
                continue;
            }
            if (std::string("+-*/^(),").find(character) != std::string::npos) {
                Token token;
                token.kind = Token::Kind::symbol;
                token.text = std::string(1, character);
                tokens_.push_back(std::move(token));
                ++index;
                continue;
            }
            error_ = "unsupported character in expression: '" + std::string(1, character) + "'";
            return;
        }
        tokens_.push_back({Token::Kind::end, 0.0, {}});
    }

    const Token &peek(std::size_t offset = 0) const {
        const auto index = std::min(position_ + offset, tokens_.size() - 1);
        return tokens_[index];
    }

    bool accept(const std::string &symbol) {
        if (peek().text != symbol)
            return false;
        ++position_;
        return true;
    }

    ExprPtr parseAdditive() {
        auto left = parseMultiplicative();
        if (!left)
            return nullptr;
        while (peek().text == "+" || peek().text == "-") {
            const auto operation = peek().text[0];
            ++position_;
            auto right = parseMultiplicative();
            if (!right)
                return nullptr;
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::binary;
            node->op = operation;
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parseMultiplicative() {
        auto left = parseUnary();
        if (!left)
            return nullptr;
        while (peek().text == "*" || peek().text == "/") {
            const auto operation = peek().text[0];
            ++position_;
            auto right = parseUnary();
            if (!right)
                return nullptr;
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::binary;
            node->op = operation;
            node->args.push_back(std::move(left));
            node->args.push_back(std::move(right));
            left = std::move(node);
        }
        return left;
    }

    ExprPtr parseUnary() {
        if (peek().text == "+" || peek().text == "-") {
            const auto operation = peek().text[0];
            ++position_;
            auto operand = parseUnary();
            if (!operand)
                return nullptr;
            if (operation == '+')
                return operand;
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::unary;
            node->op = '-';
            node->args.push_back(std::move(operand));
            return node;
        }
        return parsePrimary();
    }

    ExprPtr parsePrimary() {
        if (peek().kind == Token::Kind::number) {
            auto node = std::make_unique<ExprNode>();
            node->kind = ExprNode::Kind::constant;
            node->constant = peek().number;
            ++position_;
            return node;
        }
        if (accept("(")) {
            auto node = parseAdditive();
            if (!node)
                return nullptr;
            if (!accept(")")) {
                error_ = "missing closing parenthesis";
                return nullptr;
            }
            return node;
        }
        if (peek().kind != Token::Kind::identifier) {
            error_ = "expected a number, signal name, or function call";
            return nullptr;
        }
        auto node = std::make_unique<ExprNode>();
        node->text = peek().text;
        ++position_;
        if (accept("(")) {
            node->kind = ExprNode::Kind::call;
            if (!accept(")")) {
                for (;;) {
                    auto argument = parseAdditive();
                    if (!argument)
                        return nullptr;
                    node->args.push_back(std::move(argument));
                    if (accept(","))
                        continue;
                    if (!accept(")")) {
                        error_ = "missing closing parenthesis in call to " + node->text;
                        return nullptr;
                    }
                    break;
                }
            }
            return node;
        }
        node->kind = ExprNode::Kind::name;
        return node;
    }

    std::optional<std::vector<double>> evaluate(const ExprNode &node, std::vector<double> &values) {
        switch (node.kind) {
        case ExprNode::Kind::constant:
            values.assign(result_.time.size(), node.constant);
            return values;
        case ExprNode::Kind::name: {
            if (const auto *axis = axisValues(result_, node.text)) {
                values = *axis;
                return values;
            }
            const auto *trace = findSignalTrace(result_, node.text);
            if (!trace) {
                error_ = "unknown signal '" + node.text + "'";
                return std::nullopt;
            }
            values = clippedValues(*trace, result_.time.size());
            return values;
        }
        case ExprNode::Kind::unary: {
            auto operand = std::vector<double>{};
            if (!evaluate(*node.args.front(), operand))
                return std::nullopt;
            for (auto &value : operand)
                value = -value;
            values = std::move(operand);
            return values;
        }
        case ExprNode::Kind::binary: {
            auto left = std::vector<double>{};
            auto right = std::vector<double>{};
            if (!evaluate(*node.args[0], left) || !evaluate(*node.args[1], right))
                return std::nullopt;
            if (left.size() != right.size()) {
                error_ = "signals have different sample counts";
                return std::nullopt;
            }
            values.resize(left.size());
            for (std::size_t index = 0; index < left.size(); ++index) {
                switch (node.op) {
                case '+':
                    values[index] = left[index] + right[index];
                    break;
                case '-':
                    values[index] = left[index] - right[index];
                    break;
                case '*':
                    values[index] = left[index] * right[index];
                    break;
                case '/':
                    values[index] = std::abs(right[index]) <= epsilon ? std::numeric_limits<double>::quiet_NaN()
                                                                      : left[index] / right[index];
                    break;
                default:
                    error_ = "unsupported operator";
                    return std::nullopt;
                }
            }
            return values;
        }
        case ExprNode::Kind::call:
            return evaluateCall(node, values);
        }
        error_ = "unsupported expression";
        return std::nullopt;
    }

    std::optional<std::vector<double>> evaluateCall(const ExprNode &node, std::vector<double> &values) {
        const auto name = lowerNoSpace(node.text);
        if (name == "v" || name == "i") {
            if (node.args.size() != 1 || node.args.front()->kind != ExprNode::Kind::name) {
                error_ = node.text + "() expects a signal name";
                return std::nullopt;
            }
            const auto raw = node.args.front()->text;
            for (const auto &candidate : {raw, name + "(" + raw + ")"}) {
                if (const auto *trace = findSignalTrace(result_, candidate)) {
                    values = clippedValues(*trace, result_.time.size());
                    return values;
                }
            }
            error_ = "unknown signal '" + raw + "'";
            return std::nullopt;
        }
        if (name == "phase") {
            if (node.args.size() != 1) {
                error_ = "phase() expects one argument";
                return std::nullopt;
            }
            std::string signal;
            if (node.args.front()->kind == ExprNode::Kind::name) {
                signal = node.args.front()->text;
            } else {
                error_ = "phase() expects a signal name";
                return std::nullopt;
            }
            for (const auto &candidate : {signal + ".phase", "v(" + signal + ").phase", signal}) {
                if (const auto *trace = findSignalTrace(result_, candidate)) {
                    if (candidate == signal && !lowerNoSpace(trace->name).ends_with(".phase")) {
                        error_ = "no phase trace for '" + signal + "'";
                        return std::nullopt;
                    }
                    values = clippedValues(*trace, result_.time.size());
                    return values;
                }
            }
            error_ = "no phase trace for '" + signal + "'";
            return std::nullopt;
        }
        std::vector<std::vector<double>> arguments;
        for (const auto &argument : node.args) {
            std::vector<double> resolved;
            if (!evaluate(*argument, resolved))
                return std::nullopt;
            arguments.push_back(std::move(resolved));
        }
        if (arguments.empty()) {
            error_ = name + "() needs at least one argument";
            return std::nullopt;
        }
        const auto count = arguments.front().size();
        for (const auto &argument : arguments) {
            if (argument.size() != count) {
                error_ = "function arguments have different sample counts";
                return std::nullopt;
            }
        }
        values.resize(count);
        if (name == "derivative" || name == "integral") {
            if (arguments.size() != 1 || count == 0) {
                error_ = name + "() expects one signal";
                return std::nullopt;
            }
            if (result_.time.size() < count || count < 2) {
                error_ = name + "() needs at least two samples";
                return std::nullopt;
            }
            const auto &axis = result_.time;
            if (name == "derivative") {
                for (std::size_t index = 0; index < count; ++index) {
                    const auto first = index == 0 ? 0 : index - 1;
                    const auto second = index + 1 >= count ? count - 1 : index + 1;
                    const auto span = axis[second] - axis[first];
                    values[index] = std::abs(span) <= epsilon
                                        ? std::numeric_limits<double>::quiet_NaN()
                                        : (arguments.front()[second] - arguments.front()[first]) / span;
                }
            } else {
                values[0] = 0.0;
                for (std::size_t index = 1; index < count; ++index) {
                    const auto span = axis[index] - axis[index - 1];
                    values[index] =
                        values[index - 1] + 0.5 * (arguments.front()[index] + arguments.front()[index - 1]) * span;
                }
            }
            return values;
        }
        if (name == "min" || name == "max") {
            if (arguments.size() != 2) {
                error_ = name + "() expects two arguments";
                return std::nullopt;
            }
            for (std::size_t index = 0; index < count; ++index)
                values[index] = name == "min" ? std::min(arguments[0][index], arguments[1][index])
                                              : std::max(arguments[0][index], arguments[1][index]);
            return values;
        }
        if (arguments.size() != 1) {
            error_ = name + "() expects one argument";
            return std::nullopt;
        }
        for (std::size_t index = 0; index < count; ++index) {
            const auto value = arguments.front()[index];
            if (name == "abs" || name == "mag")
                values[index] = std::abs(value);
            else if (name == "sqrt")
                values[index] = value < 0.0 ? std::numeric_limits<double>::quiet_NaN() : std::sqrt(value);
            else if (name == "exp")
                values[index] = std::exp(value);
            else if (name == "ln" || name == "log")
                values[index] = value <= 0.0 ? std::numeric_limits<double>::quiet_NaN() : std::log(value);
            else if (name == "log10")
                values[index] = value <= 0.0 ? std::numeric_limits<double>::quiet_NaN() : std::log10(value);
            else if (name == "db")
                values[index] = std::isfinite(value)
                                    ? std::max(decibelFloor, 20.0 * std::log10(std::max(std::abs(value), epsilon)))
                                    : std::numeric_limits<double>::quiet_NaN();
            else {
                error_ = "unknown function '" + node.text + "'";
                return std::nullopt;
            }
        }
        return values;
    }

    std::string expression_;
    const SimulationResult &result_;
    std::vector<Token> tokens_;
    std::size_t position_ = 0;
    std::string error_;
};

} // namespace

const char *waveformViewName(WaveformView view) {
    switch (view) {
    case WaveformView::linear:
        return "linear";
    case WaveformView::decibel:
        return "db";
    case WaveformView::phase:
        return "phase";
    }
    return "linear";
}

std::optional<WaveformView> parseWaveformView(const std::string &text) {
    auto normalized = lowerNoSpace(text);
    if (normalized.empty() || normalized == "linear" || normalized == "lin")
        return WaveformView::linear;
    if (normalized == "db" || normalized == "decibel" || normalized == "magnitude_db")
        return WaveformView::decibel;
    if (normalized == "phase" || normalized == "deg" || normalized == "degrees")
        return WaveformView::phase;
    return std::nullopt;
}

std::vector<double> waveformViewValues(const SimulationTrace &trace, WaveformView view,
                                       const SimulationResult &result) {
    switch (view) {
    case WaveformView::linear:
        return trace.values;
    case WaveformView::decibel:
        return decibelValues(trace.values);
    case WaveformView::phase: {
        if (const auto *phase = findSignalTrace(result, trace.name + ".phase"))
            return phase->values;
        // `v(out)` and `out` name the same node, so tolerate either spelling
        // when locating the companion phase trace.
        auto stripped = trace.name;
        const auto normalized = lowerNoSpace(stripped);
        if (normalized.size() > 3 && normalized.back() == ')' &&
            (normalized.rfind("v(", 0) == 0 || normalized.rfind("i(", 0) == 0)) {
            // Preserve the original spelling after removing the wrapper.
            const auto open = stripped.find('(');
            const auto close = stripped.rfind(')');
            if (open != std::string::npos && close > open)
                stripped = stripped.substr(open + 1, close - open - 1);
        }
        if (const auto *phase = findSignalTrace(result, stripped + ".phase"))
            return phase->values;
        if (normalized.ends_with(".phase"))
            return trace.values;
        return {};
    }
    }
    return trace.values;
}

WaveformSpectrum computeSpectrum(const std::vector<double> &x, const std::vector<SimulationTrace> &traces,
                                 const WaveformFftOptions &options) {
    WaveformSpectrum spectrum;
    if (x.size() < 4 || traces.empty()) {
        spectrum.warnings.push_back("FFT needs at least four samples and one trace");
        return spectrum;
    }
    const auto count = std::min<std::size_t>(x.size(), 1 << 20);
    std::vector<double> axis(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(count));
    if (!std::is_sorted(axis.begin(), axis.end())) {
        if (std::is_sorted(axis.rbegin(), axis.rend())) {
            std::reverse(axis.begin(), axis.end());
        } else {
            spectrum.warnings.push_back("FFT needs a monotonic x axis");
            return spectrum;
        }
    }
    const auto span = axis.back() - axis.front();
    if (!(span > 0.0) || !std::isfinite(span)) {
        spectrum.warnings.push_back("FFT needs a finite, non-zero x span");
        return spectrum;
    }
    const auto sampleSpacing = span / static_cast<double>(count - 1);
    if (!(sampleSpacing > 0.0) || !std::isfinite(sampleSpacing)) {
        spectrum.warnings.push_back("FFT could not determine the sample spacing");
        return spectrum;
    }
    // Flag non-uniform spacing once; the resampling below handles it.
    bool nonUniform = false;
    for (std::size_t index = 1; index < count; ++index) {
        if (std::abs((axis[index] - axis[index - 1]) - sampleSpacing) > std::abs(sampleSpacing) * 1e-3) {
            nonUniform = true;
            break;
        }
    }
    if (nonUniform)
        spectrum.warnings.push_back("FFT resampled non-uniform samples onto a uniform grid");

    const auto fftSize = nextPowerOfTwo(options.points > 0 ? options.points : count);
    const auto window = lowerNoSpace(options.window);
    double coherentGain = 0.0;
    std::vector<double> coefficients(count, 1.0);
    for (std::size_t index = 0; index < count; ++index) {
        coefficients[index] = windowCoefficient(window, index, count);
        coherentGain += coefficients[index];
    }
    coherentGain /= static_cast<double>(count);
    if (coherentGain <= 0.0)
        coherentGain = 1.0;

    const auto bins = options.oneSided ? fftSize / 2 + 1 : fftSize;
    spectrum.frequency.resize(bins);
    const auto sampleRate = 1.0 / sampleSpacing;
    for (std::size_t bin = 0; bin < bins; ++bin)
        spectrum.frequency[bin] = static_cast<double>(bin) * sampleRate / static_cast<double>(fftSize);

    for (const auto &trace : traces) {
        if (trace.values.size() < count) {
            spectrum.warnings.push_back("FFT skipped '" + trace.name + "': not enough samples");
            continue;
        }
        std::vector<double> resampled(count, 0.0);
        std::size_t hint = 0;
        const auto localAxis = axis;
        for (std::size_t index = 0; index < count; ++index)
            resampled[index] = interpolate(localAxis, trace.values,
                                           localAxis.front() + static_cast<double>(index) * sampleSpacing, hint);

        std::vector<std::complex<double>> data(fftSize, {0.0, 0.0});
        const auto stored = std::min(count, fftSize);
        for (std::size_t index = 0; index < stored; ++index)
            data[index] = {resampled[index] * coefficients[index], 0.0};
        fftInPlace(data);

        SimulationTrace magnitude;
        magnitude.name = trace.name + " FFT";
        magnitude.values.resize(bins);
        SimulationTrace phase;
        phase.name = trace.name + " FFT.phase";
        phase.values.resize(bins);
        for (std::size_t bin = 0; bin < bins; ++bin) {
            const auto magnitudeValue = std::abs(data[bin]) *
                                        ((options.oneSided && bin > 0 && bin + 1 < bins) ? 2.0 : 1.0) /
                                        (static_cast<double>(fftSize) * coherentGain);
            magnitude.values[bin] = magnitudeValue;
            phase.values[bin] = std::arg(data[bin]) * 180.0 / std::numbers::pi;
        }
        spectrum.magnitude.push_back(std::move(magnitude));
        spectrum.phase.push_back(std::move(phase));
    }
    return spectrum;
}

bool evaluateDerivedTrace(const std::string &name, const std::string &expression, const SimulationResult &result,
                          SimulationTrace &trace, std::string *error) {
    const auto trimmed = trim(expression);
    if (trimmed.empty()) {
        if (error)
            *error = "waveform expression is empty";
        return false;
    }
    ExpressionParser parser(trimmed, result);
    std::string parseError;
    auto root = parser.parse(parseError);
    if (!root) {
        if (error)
            *error = parseError;
        return false;
    }
    std::vector<double> values;
    std::string evaluationError;
    if (!parser.resolve(*root, values, evaluationError)) {
        if (error)
            *error = evaluationError;
        return false;
    }
    trace.name = name.empty() ? trimmed : name;
    trace.values = std::move(values);
    return true;
}

std::vector<SimulationTrace> evaluateDerivedTraces(const std::vector<SimulationDerivedTrace> &specs,
                                                   const SimulationResult &result, std::vector<std::string> *errors) {
    std::vector<SimulationTrace> traces;
    for (const auto &spec : specs) {
        SimulationTrace trace;
        std::string error;
        if (!evaluateDerivedTrace(spec.name, spec.expression, result, trace, &error)) {
            if (errors)
                errors->push_back("Derived trace '" + spec.name + "' failed: " + error);
            continue;
        }
        traces.push_back(std::move(trace));
    }
    return traces;
}

std::vector<WaveformSeries> buildWaveformSeries(const SimulationResult &result,
                                                const std::vector<std::string> &signalNames,
                                                const std::vector<SimulationDerivedTrace> &derived, WaveformView view,
                                                bool fft, const WaveformFftOptions &options,
                                                std::vector<std::string> *warnings) {
    std::vector<WaveformSeries> series;
    std::vector<SimulationTrace> traces;
    const auto emitWarning = [&](const std::string &message) {
        if (warnings)
            warnings->push_back(message);
    };

    auto addSignal = [&](const std::string &signal) {
        if (const auto *axis = axisValues(result, signal)) {
            SimulationTrace trace;
            trace.name = result.xAxis.empty() ? "time" : result.xAxis;
            trace.values = *axis;
            traces.push_back(std::move(trace));
            return;
        }
        const auto *trace = findSignalTrace(result, signal);
        if (!trace) {
            emitWarning("Graph signal not found in result: " + signal);
            return;
        }
        auto copy = *trace;
        copy.values = clippedValues(*trace, result.time.size());
        traces.push_back(std::move(copy));
    };

    if (signalNames.empty()) {
        for (const auto &trace : result.traces) {
            auto copy = trace;
            copy.values = clippedValues(trace, result.time.size());
            traces.push_back(std::move(copy));
        }
    } else {
        for (const auto &signal : signalNames)
            addSignal(signal);
    }

    std::vector<std::string> derivedErrors;
    auto derivedTraces = evaluateDerivedTraces(derived, result, &derivedErrors);
    for (const auto &error : derivedErrors)
        emitWarning(error);
    for (auto &trace : derivedTraces)
        traces.push_back(std::move(trace));

    if (fft) {
        const auto spectrum = computeSpectrum(result.time, traces, options);
        for (const auto &warning : spectrum.warnings)
            emitWarning(warning);
        for (std::size_t index = 0; index < spectrum.magnitude.size(); ++index) {
            WaveformSeries entry;
            entry.name = spectrum.magnitude[index].name;
            entry.label = entry.name;
            entry.x = spectrum.frequency;
            entry.xUnit = "Hz";
            entry.yUnit = view == WaveformView::decibel ? "dB" : (view == WaveformView::phase ? "deg" : "");
            if (view == WaveformView::phase) {
                entry.y = spectrum.phase[index].values;
                entry.label += " phase";
            } else if (view == WaveformView::decibel) {
                entry.y = decibelValues(spectrum.magnitude[index].values);
                entry.label += " dB";
            } else {
                entry.y = spectrum.magnitude[index].values;
            }
            series.push_back(std::move(entry));
        }
        return series;
    }

    for (const auto &trace : traces) {
        auto values = waveformViewValues(trace, view, result);
        if (values.empty()) {
            emitWarning("No phase data for signal: " + trace.name);
            continue;
        }
        WaveformSeries entry;
        entry.name = trace.name;
        entry.label = trace.name;
        if (view == WaveformView::decibel)
            entry.label += " dB";
        else if (view == WaveformView::phase)
            entry.label += " phase";
        entry.x = result.time;
        entry.y = std::move(values);
        entry.xUnit = result.xUnit;
        entry.yUnit = view == WaveformView::decibel ? "dB" : (view == WaveformView::phase ? "deg" : "");
        series.push_back(std::move(entry));
    }
    return series;
}

} // namespace ice
