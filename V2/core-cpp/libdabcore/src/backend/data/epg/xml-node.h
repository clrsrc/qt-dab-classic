// DAB Classic v3 – minimaler XML-Baum als Ersatz fuer QDomDocument/QDomElement
// im portierten epg-compiler. save() erzeugt exakt die Textform von
// QDomDocument::toString (1): Einrueckung 1 Leerzeichen je Ebene, Attribute
// in Namensreihenfolge (QMap-Ordnung), Text-Kinder inline, keine
// XML-Deklaration, Zeilenende "\n". Damit bleiben die Dateien
// <yyyyMMdd>_<SId>_SI.xml zu den v1-Dateien (Qt-DAB-files/<EId>/) formatgleich.
#pragma once

#include <map>
#include <string>
#include <vector>

class XmlNode {
public:
    XmlNode() = default;
    explicit XmlNode(std::string elementName) : name_(std::move(elementName)) {}
    static XmlNode text(std::string value) { XmlNode n; n.isText_ = true; n.value_ = std::move(value); return n; }

    bool isText() const { return isText_; }
    const std::string& name() const { return name_; }

    // QDomElement::setAttribute (String / int)
    void setAttribute(const std::string& attr, const std::string& value) { attrs_[attr] = value; }
    void setAttribute(const std::string& attr, int value) { attrs_[attr] = std::to_string(value); }
    bool hasAttribute(const std::string& attr) const { return attrs_.count(attr) != 0; }
    std::string attribute(const std::string& attr) const { auto it = attrs_.find(attr); return it == attrs_.end() ? "" : it->second; }

    // QDomNode::appendChild
    void appendChild(XmlNode child) { children_.push_back(std::move(child)); }
    void appendText(std::string value) { appendChild(text(std::move(value))); }
    const std::vector<XmlNode>& children() const { return children_; }

    // QDomDocument::toString (1) fuer ein Dokument, dessen einziger Knoten
    // dieses Element ist.
    std::string toString() const;

private:
    void save(std::string& out, int depth, bool prevIsText, bool nextIsText) const;
    bool isText_ = false;
    std::string name_;
    std::string value_;                          // nur Textknoten
    std::map<std::string, std::string> attrs_;   // QMap<QString,..>: sortiert
    std::vector<XmlNode> children_;
};
