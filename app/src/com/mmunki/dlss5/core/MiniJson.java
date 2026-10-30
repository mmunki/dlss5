package com.mmunki.dlss5.core;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * A tiny JSON reader/writer.  The app and the daemon exchange a flat config
 * object, so pulling in a full JSON library would only add weight.  The
 * parser handles objects, arrays, strings with escapes, numbers, booleans
 * and null, exactly the subset both ends write.
 */
public final class MiniJson {

    public static final class ParseException extends Exception {
        public ParseException(String message) {
            super(message);
        }
    }

    private MiniJson() {
    }

    public static Object parse(String text) throws ParseException {
        if (text == null) {
            throw new ParseException("empty input");
        }
        Parser parser = new Parser(text);
        Object value = parser.value(0);
        parser.skipWhitespace();
        if (!parser.atEnd()) {
            throw new ParseException("trailing data at " + parser.position);
        }
        return value;
    }

    @SuppressWarnings("unchecked")
    public static Map<String, Object> parseObject(String text) throws ParseException {
        Object value = parse(text);
        if (!(value instanceof Map)) {
            throw new ParseException("expected an object");
        }
        return (Map<String, Object>) value;
    }

    public static String quote(String value) {
        StringBuilder out = new StringBuilder(value.length() + 2);
        out.append('"');
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if (c < 0x20) {
                        out.append(String.format("\\u%04x", (int) c));
                    } else {
                        out.append(c);
                    }
            }
        }
        out.append('"');
        return out.toString();
    }

    public static boolean bool(Map<String, Object> object, String key, boolean fallback) {
        Object value = object == null ? null : object.get(key);
        if (value instanceof Boolean) {
            return (Boolean) value;
        }
        if (value instanceof Number) {
            return ((Number) value).doubleValue() != 0.0;
        }
        return fallback;
    }

    public static double number(Map<String, Object> object, String key, double fallback) {
        Object value = object == null ? null : object.get(key);
        if (value instanceof Number) {
            return ((Number) value).doubleValue();
        }
        if (value instanceof Boolean) {
            return ((Boolean) value) ? 1.0 : 0.0;
        }
        return fallback;
    }

    public static String string(Map<String, Object> object, String key, String fallback) {
        Object value = object == null ? null : object.get(key);
        return value instanceof String ? (String) value : fallback;
    }

    @SuppressWarnings("unchecked")
    public static List<Object> list(Map<String, Object> object, String key) {
        Object value = object == null ? null : object.get(key);
        return value instanceof List ? (List<Object>) value : null;
    }

    private static final class Parser {
        private final String text;
        private int position;

        Parser(String text) {
            this.text = text;
        }

        boolean atEnd() {
            return position >= text.length();
        }

        void skipWhitespace() {
            while (!atEnd() && Character.isWhitespace(text.charAt(position))) {
                position++;
            }
        }

        Object value(int depth) throws ParseException {
            if (depth > 32) {
                throw new ParseException("nesting too deep");
            }
            skipWhitespace();
            if (atEnd()) {
                throw new ParseException("unexpected end of input");
            }
            char c = text.charAt(position);
            switch (c) {
                case '{': return object(depth);
                case '[': return array(depth);
                case '"': return string();
                case 't': expect("true"); return Boolean.TRUE;
                case 'f': expect("false"); return Boolean.FALSE;
                case 'n': expect("null"); return null;
                default: return number();
            }
        }

        private void expect(String literal) throws ParseException {
            if (!text.startsWith(literal, position)) {
                throw new ParseException("bad literal at " + position);
            }
            position += literal.length();
        }

        private Map<String, Object> object(int depth) throws ParseException {
            Map<String, Object> map = new LinkedHashMap<String, Object>();
            position++; // {
            skipWhitespace();
            if (!atEnd() && text.charAt(position) == '}') {
                position++;
                return map;
            }
            while (true) {
                skipWhitespace();
                String key = string();
                skipWhitespace();
                if (atEnd() || text.charAt(position) != ':') {
                    throw new ParseException("expected : at " + position);
                }
                position++;
                map.put(key, value(depth + 1));
                skipWhitespace();
                if (atEnd()) {
                    throw new ParseException("unterminated object");
                }
                char c = text.charAt(position++);
                if (c == '}') {
                    return map;
                }
                if (c != ',') {
                    throw new ParseException("expected , or } at " + (position - 1));
                }
            }
        }

        private List<Object> array(int depth) throws ParseException {
            List<Object> list = new ArrayList<Object>();
            position++; // [
            skipWhitespace();
            if (!atEnd() && text.charAt(position) == ']') {
                position++;
                return list;
            }
            while (true) {
                list.add(value(depth + 1));
                skipWhitespace();
                if (atEnd()) {
                    throw new ParseException("unterminated array");
                }
                char c = text.charAt(position++);
                if (c == ']') {
                    return list;
                }
                if (c != ',') {
                    throw new ParseException("expected , or ] at " + (position - 1));
                }
            }
        }

        private String string() throws ParseException {
            if (atEnd() || text.charAt(position) != '"') {
                throw new ParseException("expected string at " + position);
            }
            position++;
            StringBuilder out = new StringBuilder();
            while (true) {
                if (atEnd()) {
                    throw new ParseException("unterminated string");
                }
                char c = text.charAt(position++);
                if (c == '"') {
                    return out.toString();
                }
                if (c != '\\') {
                    out.append(c);
                    continue;
                }
                if (atEnd()) {
                    throw new ParseException("bad escape");
                }
                char esc = text.charAt(position++);
                switch (esc) {
                    case '"': out.append('"'); break;
                    case '\\': out.append('\\'); break;
                    case '/': out.append('/'); break;
                    case 'b': out.append('\b'); break;
                    case 'f': out.append('\f'); break;
                    case 'n': out.append('\n'); break;
                    case 'r': out.append('\r'); break;
                    case 't': out.append('\t'); break;
                    case 'u':
                        if (position + 4 > text.length()) {
                            throw new ParseException("bad \\u escape");
                        }
                        out.append((char) Integer.parseInt(text.substring(position, position + 4), 16));
                        position += 4;
                        break;
                    default:
                        throw new ParseException("bad escape \\" + esc);
                }
            }
        }

        private Double number() throws ParseException {
            int start = position;
            if (!atEnd() && (text.charAt(position) == '-' || text.charAt(position) == '+')) {
                position++;
            }
            while (!atEnd()) {
                char c = text.charAt(position);
                if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E'
                        || c == '+' || c == '-') {
                    position++;
                } else {
                    break;
                }
            }
            if (start == position) {
                throw new ParseException("expected a value at " + start);
            }
            try {
                return Double.valueOf(text.substring(start, position));
            } catch (NumberFormatException e) {
                throw new ParseException("bad number at " + start);
            }
        }
    }
}
