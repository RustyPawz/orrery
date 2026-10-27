// Just enough JSON for the helper's one-line messages: objects, arrays, strings, numbers, booleans, null.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ory {

struct Json {
	enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Json> arr;
	std::map<std::string, Json> obj;

	Json () = default;
	Json (bool v) : kind (Bool), b (v) {}
	Json (int v) : kind (Num), n (v) {}
	Json (double v) : kind (Num), n (v) {}
	Json (const char* v) : kind (Str), s (v) {}
	Json (std::string v) : kind (Str), s (std::move (v)) {}
	static Json array () { Json j; j.kind = Arr; return j; }
	static Json object () { Json j; j.kind = Obj; return j; }

	Json& operator[] (const std::string& k) { kind = Obj; return obj[k]; }
	const Json& get (const std::string& k) const
	{
		static const Json none;
		auto it = obj.find (k);
		return it == obj.end () ? none : it->second;
	}
	void push (Json v) { kind = Arr; arr.push_back (std::move (v)); }
	std::string str (const std::string& def = "") const { return kind == Str ? s : def; }
	double num (double def = 0) const { return kind == Num ? n : def; }
	bool boolean (bool def = false) const { return kind == Bool ? b : def; }

	std::string dump () const
	{
		std::string o;
		write (o);
		return o;
	}

	void write (std::string& o) const
	{
		switch (kind)
		{
			case Null: o += "null"; break;
			case Bool: o += b ? "true" : "false"; break;
			case Num:
			{
				char buf[32];
				if (n == (long long)n && n > -1e15 && n < 1e15)
					snprintf (buf, sizeof buf, "%lld", (long long)n);
				else
					snprintf (buf, sizeof buf, "%.9g", n);
				o += buf;
				break;
			}
			case Str: quote (o, s); break;
			case Arr:
				o += '[';
				for (size_t i = 0; i < arr.size (); i++) { if (i) o += ','; arr[i].write (o); }
				o += ']';
				break;
			case Obj:
			{
				o += '{';
				bool first = true;
				for (auto& [k, v] : obj) { if (!first) o += ','; first = false; quote (o, k); o += ':'; v.write (o); }
				o += '}';
				break;
			}
		}
	}

	static void quote (std::string& o, const std::string& v)
	{
		o += '"';
		for (unsigned char c : v)
		{
			if (c == '"') o += "\\\"";
			else if (c == '\\') o += "\\\\";
			else if (c == '\n') o += "\\n";
			else if (c == '\r') o += "\\r";
			else if (c == '\t') o += "\\t";
			else if (c < 0x20) { char buf[8]; snprintf (buf, sizeof buf, "\\u%04x", c); o += buf; }
			else o += (char)c;
		}
		o += '"';
	}

	// returns a Null value for anything it can't read
	static Json parse (const std::string& text)
	{
		size_t i = 0;
		Json j;
		if (!value (text, i, j)) return Json ();
		return j;
	}

private:
	static void ws (const std::string& t, size_t& i) { while (i < t.size () && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++; }

	static void utf8 (std::string& o, unsigned cp)
	{
		if (cp < 0x80) o += (char)cp;
		else if (cp < 0x800) { o += (char)(0xc0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3f)); }
		else if (cp < 0x10000) { o += (char)(0xe0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3f)); o += (char)(0x80 | (cp & 0x3f)); }
		else { o += (char)(0xf0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3f)); o += (char)(0x80 | ((cp >> 6) & 0x3f)); o += (char)(0x80 | (cp & 0x3f)); }
	}

	static bool string (const std::string& t, size_t& i, std::string& out)
	{
		if (t[i] != '"') return false;
		i++;
		while (i < t.size () && t[i] != '"')
		{
			char c = t[i++];
			if (c != '\\') { out += c; continue; }
			if (i >= t.size ()) return false;
			char e = t[i++];
			switch (e)
			{
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'u':
				{
					if (i + 4 > t.size ()) return false;
					unsigned cp = strtoul (t.substr (i, 4).c_str (), nullptr, 16);
					i += 4;
					if (cp >= 0xd800 && cp < 0xdc00 && i + 6 <= t.size () && t[i] == '\\' && t[i + 1] == 'u')
					{
						unsigned lo = strtoul (t.substr (i + 2, 4).c_str (), nullptr, 16);
						i += 6;
						cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
					}
					utf8 (out, cp);
					break;
				}
				default: out += e;
			}
		}
		if (i >= t.size ()) return false;
		i++;
		return true;
	}

	static bool value (const std::string& t, size_t& i, Json& j)
	{
		ws (t, i);
		if (i >= t.size ()) return false;
		char c = t[i];
		if (c == '{')
		{
			j.kind = Obj;
			i++;
			ws (t, i);
			if (i < t.size () && t[i] == '}') { i++; return true; }
			for (;;)
			{
				ws (t, i);
				std::string k;
				if (i >= t.size () || !string (t, i, k)) return false;
				ws (t, i);
				if (i >= t.size () || t[i++] != ':') return false;
				if (!value (t, i, j.obj[k])) return false;
				ws (t, i);
				if (i >= t.size ()) return false;
				if (t[i] == ',') { i++; continue; }
				if (t[i] == '}') { i++; return true; }
				return false;
			}
		}
		if (c == '[')
		{
			j.kind = Arr;
			i++;
			ws (t, i);
			if (i < t.size () && t[i] == ']') { i++; return true; }
			for (;;)
			{
				j.arr.emplace_back ();
				if (!value (t, i, j.arr.back ())) return false;
				ws (t, i);
				if (i >= t.size ()) return false;
				if (t[i] == ',') { i++; continue; }
				if (t[i] == ']') { i++; return true; }
				return false;
			}
		}
		if (c == '"') { j.kind = Str; return string (t, i, j.s); }
		if (t.compare (i, 4, "true") == 0) { j = Json (true); i += 4; return true; }
		if (t.compare (i, 5, "false") == 0) { j = Json (false); i += 5; return true; }
		if (t.compare (i, 4, "null") == 0) { j = Json (); i += 4; return true; }
		char* end = nullptr;
		double v = strtod (t.c_str () + i, &end);
		if (end == t.c_str () + i) return false;
		i = end - t.c_str ();
		j = Json (v);
		return true;
	}
};

} // namespace ory
