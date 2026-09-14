#pragma once
#include <QString>
#include <vector>
#include <cstdint>

enum class Ink : uint8_t { Plain, Keyword, String, Comment, Number, Type, Function, Directive, Added, Removed, Hunk };
struct SyntaxSpan { qsizetype start, length; Ink ink; };
std::vector<SyntaxSpan> highlight(const QString &path, const QString &source);
uint32_t inkColor(Ink ink);
