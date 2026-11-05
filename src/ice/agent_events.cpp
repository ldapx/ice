#include "agent_events.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace ice::agent {

namespace {

QString jsonString(const nlohmann::json &object, const char *key, const QString &fallback = {}) {
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return fallback;
    return QString::fromStdString(it->get<std::string>());
}

QString scalarJsonText(const nlohmann::json &value) {
    if (value.is_string())
        return QString::fromStdString(value.get<std::string>());
    if (value.is_boolean())
        return value.get<bool>() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.is_number())
        return QString::fromStdString(value.dump());
    if (value.is_null())
        return QStringLiteral("null");
    return {};
}

} // namespace

QString formatToolArguments(const nlohmann::json &input) {
    if (!input.is_object())
        return {};
    QStringList fields;
    for (auto it = input.begin(); it != input.end(); ++it) {
        const auto &value = it.value();
        if (value.is_object() || value.is_array())
            continue;
        const auto key = QString::fromStdString(it.key());
        if (key == QStringLiteral("cwd") || key == QStringLiteral("workdir") || key == QStringLiteral("timeout") ||
            key == QStringLiteral("background"))
            continue;
        QString text = scalarJsonText(value);
        if (text.contains(QLatin1Char('\n')))
            text = text.section(QLatin1Char('\n'), 0, 0) + QChar(0x2026);
        if (text.size() > 2000)
            text = text.left(1997) + QChar(0x2026);
        fields.push_back(key + QLatin1Char('=') + text);
    }
    return fields.join(QStringLiteral(", "));
}

QString contentText(const nlohmann::json &update) {
    QString output;
    const auto contentIt = update.find("content");
    if (contentIt != update.end() && contentIt->is_array()) {
        for (const auto &item : *contentIt) {
            if (!item.is_object())
                continue;
            const auto itemType = jsonString(item, "type");
            if (itemType == QStringLiteral("content") || itemType == QStringLiteral("text")) {
                const auto inner = itemType == QStringLiteral("content") ? item.find("content") : item.find("text");
                QString chunk;
                if (itemType == QStringLiteral("text") && inner != item.end() && inner->is_string())
                    chunk = QString::fromStdString(inner->get<std::string>());
                else if (inner != item.end() && inner->is_object() &&
                         jsonString(*inner, "type") == QStringLiteral("text"))
                    chunk = jsonString(*inner, "text");
                if (!chunk.isEmpty()) {
                    if (!output.isEmpty())
                        output += QLatin1Char('\n');
                    output += chunk;
                }
            }
        }
    }
    const auto rawOutputIt = update.find("rawOutput");
    if (rawOutputIt != update.end() && rawOutputIt->is_object()) {
        const auto errorIt = rawOutputIt->find("error");
        if (errorIt != rawOutputIt->end()) {
            QString error;
            if (errorIt->is_string())
                error = QString::fromStdString(errorIt->get<std::string>());
            else if (errorIt->is_object())
                error = jsonString(*errorIt, "message");
            if (!error.isEmpty()) {
                if (!output.isEmpty())
                    output += QLatin1Char('\n');
                output += error;
            }
        }
    }
    return output;
}

QVector<AgentQuestion> questionUpdates(const nlohmann::json &update) {
    QVector<AgentQuestion> questions;
    const auto inputIt = update.find("rawInput");
    if (inputIt == update.end() || !inputIt->is_object())
        return questions;
    const auto questionsIt = inputIt->find("questions");
    if (questionsIt == inputIt->end() || !questionsIt->is_array())
        return questions;
    for (const auto &item : *questionsIt) {
        if (!item.is_object())
            continue;
        AgentQuestion question;
        question.header = jsonString(item, "header");
        question.question = jsonString(item, "question");
        const auto optionsIt = item.find("options");
        if (optionsIt != item.end() && optionsIt->is_array()) {
            for (const auto &option : *optionsIt) {
                if (!option.is_object())
                    continue;
                const auto label = jsonString(option, "label");
                if (!label.isEmpty())
                    question.options.push_back(label);
            }
        }
        questions.push_back(std::move(question));
    }
    return questions;
}

QVector<QStringList> questionAnswers(const nlohmann::json &update) {
    QVector<QStringList> answers;
    const auto outputIt = update.find("rawOutput");
    if (outputIt == update.end() || !outputIt->is_object())
        return answers;
    const auto metadataIt = outputIt->find("metadata");
    if (metadataIt == outputIt->end() || !metadataIt->is_object())
        return answers;
    const auto answersIt = metadataIt->find("answers");
    if (answersIt == metadataIt->end() || !answersIt->is_array())
        return answers;
    for (const auto &answer : *answersIt) {
        QStringList selected;
        if (answer.is_string()) {
            selected.push_back(QString::fromStdString(answer.get<std::string>()));
        } else if (answer.is_array()) {
            for (const auto &entry : answer) {
                if (entry.is_string())
                    selected.push_back(QString::fromStdString(entry.get<std::string>()));
            }
        }
        answers.push_back(std::move(selected));
    }
    return answers;
}

void applyQuestionAnswers(QVector<AgentQuestion> &questions, const QVector<QStringList> &answers) {
    const auto count = std::min(static_cast<std::size_t>(questions.size()), static_cast<std::size_t>(answers.size()));
    for (std::size_t index = 0; index < count; ++index)
        questions[static_cast<std::size_t>(index)].answers = answers[static_cast<std::size_t>(index)];
}

AgentToolUpdate toolUpdateFromJson(const nlohmann::json &update) {
    AgentToolUpdate result;
    result.id = jsonString(update, "toolCallId");
    result.title = jsonString(update, "title");
    result.kind = jsonString(update, "kind");
    result.status = jsonString(update, "status");
    result.output = contentText(update);
    result.questions = questionUpdates(update);
    result.answers = questionAnswers(update);

    const auto inputIt = update.find("rawInput");
    if (inputIt != update.end() && inputIt->is_object())
        result.command = jsonString(*inputIt, "command");

    const auto outputIt = update.find("rawOutput");
    if (outputIt != update.end() && outputIt->is_object()) {
        const auto metadataIt = outputIt->find("metadata");
        if (metadataIt != outputIt->end() && metadataIt->is_object()) {
            const auto callsIt = metadataIt->find("toolCalls");
            if (callsIt != metadataIt->end() && callsIt->is_array()) {
                for (const auto &call : *callsIt) {
                    if (!call.is_object())
                        continue;
                    AgentToolInvocation invocation;
                    invocation.name = jsonString(call, "tool");
                    if (invocation.name.isEmpty())
                        invocation.name = jsonString(call, "name");
                    invocation.status = jsonString(call, "status");
                    if (invocation.status.isEmpty())
                        invocation.status = result.status;
                    const auto callInputIt = call.find("input");
                    if (callInputIt != call.end() && callInputIt->is_object())
                        invocation.arguments = formatToolArguments(*callInputIt);
                    const auto callErrorIt = call.find("error");
                    if (callErrorIt != call.end()) {
                        if (callErrorIt->is_string())
                            invocation.error = QString::fromStdString(callErrorIt->get<std::string>());
                        else if (callErrorIt->is_object())
                            invocation.error = jsonString(*callErrorIt, "message");
                    }
                    result.invocations.push_back(std::move(invocation));
                }
            }
        }
    }

    if (result.invocations.isEmpty() && result.questions.isEmpty() && result.answers.isEmpty() &&
        result.command.isEmpty() && !result.title.isEmpty() && result.title != QStringLiteral("execute") &&
        inputIt != update.end() && inputIt->is_object()) {
        AgentToolInvocation invocation;
        invocation.name = result.title;
        invocation.status = result.status;
        invocation.arguments = formatToolArguments(*inputIt);
        result.invocations.push_back(std::move(invocation));
    }
    return result;
}

} // namespace ice::agent
