// DAB Classic v3 – siehe xml-node.h. Die Serialisierung folgt
// QDomElementPrivate::save / QDomTextPrivate::save / encodeText (qdom.cpp).
#include "xml-node.h"

namespace {

// qdom.cpp encodeText: '<' -> &lt;, '&' -> &amp;, '"' -> &quot; (nur
// Attribute), "]]>" -> ]]&gt;, in Attributen \n \r \t als &#xa; &#xd; &#x9;,
// in Textknoten \r als &#xd;. Abweichend von v1 werden Steuerzeichen
// (< 0x20 ausser Tab/LF/CR, die in XML 1.0 nicht erlaubt sind) verworfen,
// damit das Ergebnis wohlgeformt bleibt (v1 schrieb sie ungefiltert).
std::string encodeText(const std::string& str, bool encodeQuotes, bool performAVN, bool encodeEOLs) {
    std::string out;
    out.reserve(str.size() + 16);
    for (size_t i = 0; i < str.size(); ++i) {
        const char c = str[i];
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
        if (c == '<') out += "&lt;";
        else if (encodeQuotes && c == '"') out += "&quot;";
        else if (c == '&') out += "&amp;";
        else if (c == '>' && i >= 2 && str[i - 1] == ']' && str[i - 2] == ']') out += "&gt;";
        else if (performAVN && (c == '\n' || c == '\r' || c == '\t')) {
            out += c == '\n' ? "&#xa;" : c == '\r' ? "&#xd;" : "&#x9;";
        }
        else if (encodeEOLs && c == '\r') out += "&#xd;";
        else out += c;
    }
    return out;
}

} // namespace

std::string XmlNode::toString() const {
    std::string out;
    save(out, 0, false, false);
    return out;
}

void XmlNode::save(std::string& out, int depth, bool prevIsText, bool nextIsText) const {
    if (isText_) {
        out += encodeText(value_, false, false, true);
        return;
    }
    if (!prevIsText) out.append(static_cast<size_t>(depth), ' ');
    out += '<';
    out += name_;
    for (const auto& a : attrs_) {
        out += ' ';
        out += a.first;
        out += "=\"";
        out += encodeText(a.second, true, true, false);
        out += '"';
    }
    if (!children_.empty()) {
        out += '>';
        if (!children_.front().isText()) out += '\n';
        for (size_t i = 0; i < children_.size(); ++i) {
            const bool p = i > 0 && children_[i - 1].isText();
            const bool n = i + 1 < children_.size() && children_[i + 1].isText();
            children_[i].save(out, depth + 1, p, n);
        }
        if (!children_.back().isText()) out.append(static_cast<size_t>(depth), ' ');
        out += "</";
        out += name_;
        out += '>';
    } else {
        out += "/>";
    }
    if (!nextIsText) out += '\n';
}
