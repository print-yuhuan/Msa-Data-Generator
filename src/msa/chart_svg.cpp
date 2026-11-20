#include "chart_svg.hpp"

namespace msa::charts {
std::string xml_text(const std::string &value) {
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            // XML 1.0 also excludes U+FFFE and U+FFFF in otherwise valid UTF-8.
            if (value.compare(i, 3, "\xEF\xBF\xBE") == 0 ||
                value.compare(i, 3, "\xEF\xBF\xBF") == 0) {
                out += "�";
                i += 2;
            } else if (static_cast<unsigned char>(c) < 32 && c != '\t' && c != '\r' && c != '\n')
                out += "�";
            else
                out += c;
        }
    }
    return out;
}

void write_svg_items(std::ostream &out, const Scene &scene, const std::string &prefix) {
    unsigned textIndex = 0;
    out << "<g font-family=\"'Noto Sans CJK SC','Microsoft YaHei UI',sans-serif\">\n";
    for (const auto &p : scene.items) {
        switch (p.type) {
        case Primitive::Type::Line:
            out << "<line x1=\"" << p.x << "\" y1=\"" << p.y << "\" x2=\"" << p.x2 << "\" y2=\""
                << p.y2 << "\" stroke=\"" << p.color << "\" stroke-width=\"" << p.stroke_width
                << '"' << (p.dashed ? " stroke-dasharray=\"5 4\"" : "") << "/>";
            break;
        case Primitive::Type::Rect:
            out << "<rect x=\"" << p.x << "\" y=\"" << p.y << "\" width=\"" << p.x2
                << "\" height=\"" << p.y2 << "\" fill=\"" << p.color << "\"/>";
            break;
        case Primitive::Type::Circle:
            out << "<circle cx=\"" << p.x << "\" cy=\"" << p.y << "\" r=\"" << p.x2 << "\" fill=\""
                << p.color << "\"/>";
            break;
        case Primitive::Type::Text: {
            const bool center = p.align == Primitive::Align::Center;
            const bool right = p.align == Primitive::Align::Right;
            const auto id = prefix + "-t" + std::to_string(++textIndex);
            out << "<clipPath id=\"" << id << "\"><rect x=\"" << p.x << "\" y=\"" << p.y
                << "\" width=\"" << p.x2 << "\" height=\"" << p.y2 << "\"/></clipPath>";
            out << "<text clip-path=\"url(#" << id << ")\" x=\""
                << p.x + (center  ? p.x2 / 2
                          : right ? p.x2
                                  : 0)
                << "\" y=\"" << p.y + (p.y2 + p.font_size * .68f) / 2 << "\" text-anchor=\""
                << (center  ? "middle"
                    : right ? "end"
                            : "start")
                << "\" font-size=\"" << p.font_size << "\" fill=\"" << p.color << "\">"
                << xml_text(p.text) << "</text>";
            break;
        }
        }
        out << '\n';
    }
    out << "</g>\n";
}
} // namespace msa::charts
