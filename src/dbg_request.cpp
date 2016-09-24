#include "dbg_impl.h"
#include "dbg_protocol.h"
#include "dbg_network.h"	
#include "dbg_variables.h"	
#include "dbg_format.h"

namespace vscode
{
	static fs::path get_path(const rapidjson::Value& value)
	{
		assert(value.IsString());
		std::string path = value.Get<std::string>();
		std::transform(path.begin(), path.end(), path.begin(), tolower);
		return fs::path(path);
	}

	void debugger_impl::set_state(state state)
	{
		if (state_ == state) return;
		state_ = state;
		switch (state_)
		{
		case state::initialized:
			event_initialized();
			open();
			event_output("console", "Debugger initialized\n");
			break;
		case state::terminated:
			event_terminated();
			close();
			break;
		default:
			break;
		}
	}

	bool debugger_impl::is_state(state state)
	{
		return state_ == state;
	}

	void debugger_impl::set_step(step step)
	{
		step_ = step;
	}

	bool debugger_impl::is_step(step step)
	{
		return step_ == step;
	}

	void debugger_impl::step_in()
	{
		set_state(state::stepping);
		set_step(step::in);
		stepping_stacklevel_ = -1000;
		stepping_lua_state_ = 0;
	}

	void debugger_impl::step_over(lua_State* L, lua_Debug* ar)
	{
		set_state(state::stepping);
		set_step(step::over);
		stepping_stacklevel_ = stacklevel_;
		stepping_lua_state_ = L;
	}

	void debugger_impl::step_out(lua_State* L, lua_Debug* ar)
	{
		set_state(state::stepping);
		set_step(step::out);
		stepping_stacklevel_ = stacklevel_ - 1;
		stepping_lua_state_ = L;
	}

	bool debugger_impl::check_step(lua_State* L, lua_Debug* ar)
	{
		return stepping_lua_state_ == L && stepping_stacklevel_ >= stacklevel_;
	}

	bool debugger_impl::check_breakpoint(lua_State *L, lua_Debug *ar)
	{
		if (ar->currentline > 0)
		{
			if (breakpoints_.has(ar->currentline))
			{
				if (lua_getinfo(L, "S", ar))
				{
					bp_source* src = breakpoints_.get(ar->source, pathconvert_, *custom_);
					if (src && breakpoints_.has(src, ar->currentline, L, ar))
					{
						step_in();
						return true;
					}
				}
			}
		}
		return false;
	}

	bool debugger_impl::request_initialize(rprotocol& req) {
		if (!is_state(state::birth)) {
			response_error(req, "already initialized");
			return false;
		}
		response_initialized(req);
		set_state(state::initialized);
		return false;
	}

	bool debugger_impl::request_launch(rprotocol& req) {
		lua_State *L = GL;
		if (!is_state(state::initialized)) {
			response_error(req, "not initialized or unexpected state");
			return false;
		}
		auto& args = req["arguments"];
		if (!args.HasMember("program") || !args["program"].IsString()) {
			response_error(req, "Launch failed");
			return false;
		}
		bool stopOnEntry = true;
		if (args.HasMember("stopOnEntry") && args["stopOnEntry"].IsBool()) {
			stopOnEntry = args["stopOnEntry"].GetBool();
		}
		if (args.HasMember("path") && args["cpath"].IsString())
		{
			std::string path = get_path(args["path"]);
			lua_getglobal(L, "package");
			lua_pushlstring(L, path.data(), path.size());
			lua_setfield(L, -2, "path");
			lua_pop(L, 1);
		}
		if (args.HasMember("cpath") && args["cpath"].IsString())
		{
			std::string path = get_path(args["cpath"]);
			lua_getglobal(L, "package");
			lua_pushlstring(L, path.data(), path.size());
			lua_setfield(L, -2, "cpath");
			lua_pop(L, 1);
		}

		if (args.HasMember("cwd") && args["cwd"].IsString()) {
			workingdir_ = get_path(args["cwd"]);
			fs::current_path(workingdir_);
		}
		fs::path program = get_path(args["program"]);
		int status = luaL_loadfile(L, program.file_string().c_str());
		if (status != LUA_OK) {
			event_output("console", format("Failed to launch %s due to error: %s\n", program, lua_tostring(L, -1)));
			response_error(req, "Launch failed");
			lua_pop(L, 1);
			return false;
		}
		else
		{
			response_success(req);
		}

		event_thread(true);

		if (stopOnEntry)
		{
			set_state(state::stepping);
			event_stopped("entry");
		}
		else
		{
			set_state(state::running);
		}

		open();
		if (lua_pcall(L, 0, 0, 0))
		{
			event_output("console", format("Program terminated with error: %s\n", lua_tostring(L, -1)));
			lua_pop(L, 1);
		}
		set_state(state::terminated);
		return false;
	}

	bool debugger_impl::request_attach(rprotocol& req)
	{
		if (!is_state(state::initialized)) {
			response_error(req, "not initialized or unexpected state");
			return false;
		}
		auto& args = req["arguments"];
		if (!args.HasMember("program") || !args["program"].IsString()) {
			response_error(req, "Launch failed");
			return false;
		}
		bool stopOnEntry = true;
		if (args.HasMember("stopOnEntry") && args["stopOnEntry"].IsBool()) {
			stopOnEntry = args["stopOnEntry"].GetBool();
		}

		if (args.HasMember("cwd") && args["cwd"].IsString()) {
			workingdir_ = get_path(args["cwd"]);
			fs::current_path(workingdir_);
		}

		response_success(req);
		event_thread(true);

		if (stopOnEntry)
		{
			set_state(state::stepping);
			event_stopped("entry");
		}
		else
		{
			set_state(state::running);
		}
		open();
		return !stopOnEntry;
	}

	bool debugger_impl::request_thread(rprotocol& req, lua_State* L, lua_Debug *ar) {
		response_thread(req);
		return false;
	}


	static intptr_t ensure_value_fits_in_mantissa(intptr_t sourceReference) {
		assert(sourceReference <= 9007199254740991);
		return sourceReference;
	}

	bool debugger_impl::request_stack_trace(rprotocol& req, lua_State* L, lua_Debug *ar) {
		response_success(req, [&](wprotocol& res)
		{
			auto& args = req["arguments"];
			int levels = args["levels"].GetInt();
			lua_Debug entry;
			int depth = 0;
			for (auto _ : res("stackFrames").Array())
			{
				while (lua_getstack(L, depth, &entry) && depth < levels)
				{
					for (auto _ : res.Object())
					{
						int status = lua_getinfo(L, "Sln", &entry);
						assert(status);
						const char *src = entry.source;
						if (*src == '@')
						{
							src++;	
							fs::path path(src);
							if (path.is_complete())
							{
								path = path_uncomplete(path, fs::current_path<fs::path>());
							}
							path = fs::complete(path, workingdir_);
							fs::path name = path.filename();
							for (auto _ : res("source").Object())
							{
								res("name").String(name.string());
								res("path").String(path.string());
								res("sourceReference").Int64(0);
							}
						}
						else if (memcmp(src, "=[C]", 4) == 0)
						{
							for (auto _ : res("source").Object())
							{
								res("name").String("<C function>");
								res("sourceReference").Int64(-1);
							}
						}
						else if (*src == '=')
						{
							std::string client_path;
							custom::result r =  pathconvert_.get_or_eval(src, client_path, *custom_);
							if (r == custom::result::sucess || r == custom::result::sucess_once)
							{
								fs::path path = fs::complete(fs::path(client_path), workingdir_);
								fs::path name = path.filename();
								for (auto _ : res("source").Object())
								{
									res("name").String(name.string());
									res("path").String(path.string());
									res("sourceReference").Int64(0);
								}
							}
						}
						else
						{
							intptr_t reference = ensure_value_fits_in_mantissa((intptr_t)src);
							stack_.push_back({ depth, reference });
							for (auto _ : res("source").Object())
							{
								res("sourceReference").Int64(reference);
							}
						}

						res("id").Int(depth);
						res("column").Int(1);
						res("name").String(entry.name ? entry.name : "?");
						res("line").Int(entry.currentline);
						depth++;
					}
				}
			}
			res("totalFrames").Int(depth);
		});
		return false;
	}

	bool debugger_impl::request_source(rprotocol& req, lua_State* L, lua_Debug *ar) {
		auto& args = req["arguments"];
		lua_Debug entry;
		int64_t sourceReference = args["sourceReference"].GetInt64();
		int depth = -1;
		for (auto e : stack_) {
			if (e.reference == sourceReference) {
				depth = e.depth;
				break;
			}
		}
		if (lua_getstack(L, depth, &entry)) {
			int status = lua_getinfo(L, "Sln", &entry);
			if (status) {
				const char *src = entry.source;
				if (*src != '@' && *src != '=') {
					response_source(req, src);
					return false;
				}
			}
		}
		response_source(req, "Source not available");
		return false;
	}

	bool debugger_impl::request_set_breakpoints(rprotocol& req)
	{
		auto& args = req["arguments"];
		auto& source = args["source"];
		fs::path client_path = get_path(source["path"]);
		fs::path uncomplete_path = path_uncomplete(client_path, workingdir_);
		breakpoints_.clear(uncomplete_path);

		std::vector<size_t> lines;
		for (auto& m : args["breakpoints"].GetArray())
		{
			size_t line = m["line"].GetUint();
			lines.push_back(line);
			if (!m.HasMember("condition"))
			{
				breakpoints_.insert(uncomplete_path, line);
			}
			else
			{
				breakpoints_.insert(uncomplete_path, line, m["condition"].Get<std::string>());
			}
		}

		response_success(req, [&](wprotocol& res)
		{
			for (size_t d : res("breakpoints").Array(lines.size()))
			{
				for (auto _ : res.Object())
				{
					res("verified").Bool(true);
					for (auto _ : res("source").Object())
					{
						res("path").String(client_path.file_string());
					}
					res("line").Int(lines[d]);
				}
			}
		});
		return false;
	}

	bool debugger_impl::request_scopes(rprotocol& req, lua_State* L, lua_Debug *ar) {
		auto& args = req["arguments"];
		lua_Debug entry;
		int depth = args["frameId"].GetInt();
		if (!lua_getstack(L, depth, &entry)) {
			response_error(req, "Error retrieving stack frame");
			return false;
		}

		response_success(req, [&](wprotocol& res)
		{
			for (auto _ : res("scopes").Array())
			{
				int status = lua_getinfo(L, "u", &entry);
				assert(status);

				for (auto _ : res.Object())
				{
					res("name").String("Locals");
					res("variablesReference").Int64((int)var_type::local | (depth << 8));
					res("expensive").Bool(false);
				}

				if (entry.isvararg)
				{
					for (auto _ : res.Object())
					{
						res("name").String("Var Args");
						res("variablesReference").Int64((int)var_type::vararg | (depth << 8));
						res("expensive").Bool(false);
					}
				}

				for (auto _ : res.Object())
				{
					res("name").String("Upvalues");
					res("variablesReference").Int64((int)var_type::upvalue | (depth << 8));
					res("expensive").Bool(false);
				}

				for (auto _ : res.Object())
				{
					res("name").String("Globals");
					res("variablesReference").Int64((int)var_type::global | (depth << 8));
					res("expensive").Bool(false);
				}

				for (auto _ : res.Object())
				{
					res("name").String("Standard");
					res("variablesReference").Int64((int)var_type::standard | (depth << 8));
					res("expensive").Bool(false);
				}
			}
		});
		return false;
	}

	bool debugger_impl::request_variables(rprotocol& req, lua_State* L, lua_Debug *ar) {
		auto& args = req["arguments"];
		lua_Debug entry;
		int64_t var_ref = args["variablesReference"].GetInt64();
		var_type type = (var_type)(var_ref & 0xFF);
		int depth = (var_ref >> 8) & 0xFF;
		if (!lua_getstack(L, depth, &entry)) {
			response_error(req, "Error retrieving variables");
			return false;
		}

		if (type == var_type::watch)
		{
			if (!watch_.get((var_ref >> 16) & 0xFF))
			{
				response_error(req, "Error retrieving variables");
			}
		}

		response_success(req, [&](wprotocol& res)
		{
			variables resv(res, L, ar, type == var_type::watch ? -1 : 0);
			resv.push_value(type, depth, var_ref >> 16);
		});
		return false;
	}

	bool debugger_impl::request_set_variable(rprotocol& req, lua_State *L, lua_Debug *ar)
	{
		auto& args = req["arguments"];
		lua_Debug entry;
		int64_t var_ref = args["variablesReference"].GetInt64();
		var_type type = (var_type)(var_ref & 0xFF);
		int depth = (var_ref >> 8) & 0xFF;
		if (!lua_getstack(L, depth, &entry)) {
			response_error(req, "Failed set variable");
			return false;
		}
		std::string name = args["name"].Get<std::string>();
		std::string value = args["value"].Get<std::string>();
		if (!variables::set_value(L, &entry, type, depth, var_ref >> 16, name, value))
		{
			response_error(req, "Failed set variable");
			return false;
		}
		response_success(req, [&](wprotocol& res)
		{
			res("value").String(value);
		});
		return false;
	}

	bool debugger_impl::request_configuration_done(rprotocol& req)
	{
		response_success(req);
		return false;
	}

	bool debugger_impl::request_disconnect(rprotocol& req)
	{
		response_success(req);
		set_state(state::terminated);
		network_->close_session();
		return true;
	}

	bool debugger_impl::request_stepin(rprotocol& req, lua_State* L, lua_Debug *ar)
	{
		response_success(req);
		step_in();
		return true;
	}

	bool debugger_impl::request_stepout(rprotocol& req, lua_State* L, lua_Debug *ar)
	{
		response_success(req);
		step_out(L, ar);
		return true;
	}

	bool debugger_impl::request_next(rprotocol& req, lua_State* L, lua_Debug *ar)
	{
		response_success(req);
		step_over(L, ar);
		return true;
	}

	bool debugger_impl::request_continue(rprotocol& req, lua_State* L, lua_Debug *ar)
	{
		response_success(req);
		set_state(state::running);
		return true;
	}

	bool debugger_impl::request_pause(rprotocol& req)
	{
		response_success(req);
		step_in();
		return true;
	}

	bool debugger_impl::request_evaluate(rprotocol& req, lua_State *L, lua_Debug *ar)
	{
		auto& args = req["arguments"];
		auto& context = args["context"];
		int depth = args["frameId"].GetInt();
		std::string expression = args["expression"].Get<std::string>();

		lua_Debug current;
		if (!lua_getstack(L, depth, &current)) {
			response_error(req, "error stack frame");
			return false;
		}

		int nresult = 0;
		if (!evaluate(L, &current, ("return " + expression).c_str(), nresult, context == "repl"))
		{
			if (context != "repl")
			{
				response_error(req, lua_tostring(L, -1));
				lua_pop(L, 1);
				return false;
			}
			if (!evaluate(L, &current, expression.c_str(), nresult, true))
			{
				response_error(req, lua_tostring(L, -1));
				lua_pop(L, 1);
				return false;
			}
			response_success(req, [&](wprotocol& res)
			{
				res("result").String("ok");
				res("variablesReference").Int64(0);
			});
			lua_pop(L, nresult);
			return false;
		}
		std::vector<variable> rets(nresult);
		for (int i = 0; i < (int)rets.size(); ++i)
		{
			var_set_value(rets[i], L, -1 - i);
		}
		int64_t reference = 0;
		if (rets.size() == 1 && lua_type(L, -1) == LUA_TTABLE && context == "watch")
		{
			size_t pos = watch_.add();
			if (pos > 0)
			{
				reference = (int)var_type::watch | (pos << 16);
			}
		}
		lua_pop(L, nresult);
		response_success(req, [&](wprotocol& res)
		{
			if (rets.size() == 0)
			{
				res("result").String("nil");
			}
			else if (rets.size() == 1)
			{
				res("result").String(rets[0].value);
			}
			else
			{
				std::string result = rets[0].value;
				for (int i = 1; i < (int)rets.size(); ++i)
				{
					result += ", " + rets[i].value;
				}
				res("result").String(result);
			}
			res("variablesReference").Int64(reference);
		});
		return false;
	}
}
