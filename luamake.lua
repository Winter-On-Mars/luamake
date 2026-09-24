---@param b BuildCtx
function Build(b)
	-- TODO: sorry for not including lua as a part of the project, we should check the license and try to have lua as a part of the project, that way we can also include the luamake.lua file the lua project
	local lua = b:requires("lua/luamake")

	local cc = (function()
		local base = {
			std = "c++20",
			W = {
				"all",
				"pedantic",
				"conversion",
				"padded",
			},
			f = {
				"no-rtti",
			},
		}
		if args["luamake.args"] ~= nil then
			table.insert(base, args["luamake.args"])
		end
		if b.build_type() == "release" then
			table.insert(base, "-O3 -ffast-math -flto -march=native")
		elseif b.build_type() == "debug" then
			table.insert(base, "-ggdb3 -DDEBUG -fno-omit-frame-pointer")
		end
		return b.clang_bare(base)
	end)()

	local luamake = b:new_exe({
		name = args["luamake.bin"] and args["luamake.bin"] or "luamake_lua",
		root = "src/main.cpp",
		compiler = cc,
		install_dir = args["luamake.build"] ~= nil and args["luamake.build"] or "build",
		linking = { "stdc++" },
	})
	b.link_lib(lua, luamake)
	return b.install_exe(luamake)
end
