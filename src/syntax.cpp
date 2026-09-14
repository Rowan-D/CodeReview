#include "syntax.h"
#include <QFileInfo>
#include <QSet>

uint32_t inkColor(Ink ink) {
    constexpr uint32_t colors[] = {0xff28323c, 0xff8542a0, 0xff28743e, 0xff758078,
                                   0xffb06020, 0xff167b88, 0xff255ca3, 0xff986024,
                                   0xff167039, 0xffb42e38, 0xff365ea0};
    return colors[int(ink)];
}
std::vector<SyntaxSpan> highlight(const QString &path, const QString &s) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    const auto name = QFileInfo(path).fileName().toLower();
    const bool hash = QSet<QString>{"py", "sh", "bash", "zsh", "rb", "yaml", "yml", "toml", "cmake"}.contains(suffix)
                   || name == "cmakelists.txt" || name == "makefile";
    const bool python = suffix == "py";
    const bool cstyle = QSet<QString>{"c", "h", "cpp", "hpp", "cc", "cxx", "hh", "cs", "java", "js", "jsx", "ts", "tsx", "rs", "go", "swift", "kt", "css", "json", "jsonc"}.contains(suffix);
    if (!hash && !cstyle) return {};
    static const QSet<QString> keywords = {"if","else","for","while","do","switch","case","break","continue","return","class","struct","enum","namespace","using","typedef","template","typename","const","constexpr","static","public","private","protected","virtual","override","new","delete","try","catch","throw","import","from","export","default","function","let","var","async","await","yield","def","elif","in","is","not","and","or","with","as","pass","lambda","fn","pub","impl","trait","match","mut","use","mod","package","func","defer","range","interface","extends","null","nullptr","true","false","True","False","None","self","this"};
    static const QSet<QString> types = {"void","bool","char","short","int","long","float","double","auto","size_t","string","String","QString","QByteArray","Vec","Option","Result","u8","u32","u64","i32","i64","str"};
    std::vector<SyntaxSpan> out;
    auto add = [&](qsizetype start, qsizetype end, Ink ink) { out.push_back({start, end - start, ink}); };
    for (qsizetype i = 0; i < s.size();) {
        const qsizetype start = i;
        const QChar c = s[i];
        if ((hash && c == '#') || (cstyle && (i + 1 < s.size() && c == '/' && s[i + 1] == '/'))) {
            while (i < s.size() && s[i] != '\n') ++i;
            add(start, i, Ink::Comment);
        } else if (cstyle && (i + 1 < s.size() && c == '/' && s[i + 1] == '*')) {
            i += 2;
            while (i < s.size() && !(i + 1 < s.size() && s[i] == '*' && s[i + 1] == '/')) ++i;
            i = std::min(i + 2, s.size()); add(start, i, Ink::Comment);
        } else if (c == '"' || c == '\'' || ((suffix == "js" || suffix == "ts" || suffix == "jsx" || suffix == "tsx") && c == '`')) {
            const bool triple = python && (i + 2 < s.size() && s[i] == c && s[i + 1] == c && s[i + 2] == c);
            i += triple ? 3 : 1;
            while (i < s.size()) {
                if (s[i] == '\\') { i = std::min(i + 2, s.size()); continue; }
                if (triple ? (i + 2 < s.size() && s[i] == c && s[i + 1] == c && s[i + 2] == c) : s[i] == c) { i += triple ? 3 : 1; break; }
                if (!triple && c != '`' && s[i] == '\n') break;
                ++i;
            }
            add(start, i, Ink::String);
        } else if (cstyle && c == '#') {
            ++i; while (i < s.size() && (s[i].isLetter() || s[i] == ' ')) ++i;
            add(start, i, Ink::Directive);
        } else if (c.isDigit()) {
            ++i; while (i < s.size() && (s[i].isLetterOrNumber() || s[i] == '.' || s[i] == '_')) ++i;
            add(start, i, Ink::Number);
        } else if (c.isLetter() || c == '_') {
            ++i; while (i < s.size() && (s[i].isLetterOrNumber() || s[i] == '_')) ++i;
            const QString word = s.mid(start, i - start);
            qsizetype next = i; while (next < s.size() && s[next].isSpace()) ++next;
            if (keywords.contains(word)) add(start, i, Ink::Keyword);
            else if (types.contains(word) || c.isUpper()) add(start, i, Ink::Type);
            else if (next < s.size() && s[next] == '(') add(start, i, Ink::Function);
        } else ++i;
    }
    return out;
}
