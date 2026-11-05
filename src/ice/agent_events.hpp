#pragma once

#include "agent_transcript.hpp"

#include <QString>
#include <QStringList>
#include <QVector>

#include <nlohmann/json_fwd.hpp>

namespace ice::agent {

QString formatToolArguments(const nlohmann::json &input);
QString contentText(const nlohmann::json &update);
QVector<AgentQuestion> questionUpdates(const nlohmann::json &update);
QVector<QStringList> questionAnswers(const nlohmann::json &update);
void applyQuestionAnswers(QVector<AgentQuestion> &questions, const QVector<QStringList> &answers);
AgentToolUpdate toolUpdateFromJson(const nlohmann::json &update);

} // namespace ice::agent
