---@param b BuildCtx
function Build(b)
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
		--[[
		if b.build_type() == "release" then
			table.insert(base, "-O3 -ffast-math -flto -march=native")
		elseif b.build_type() == "debug" then
			table.insert(base, "-ggdb3 -DDEBUG -fno-omit-frame-pointer")
		end
    ]]
		return b.clang_bare(base)
	end)()

	local lm = b:new_exe({
		name = args["luamake.bin"] and args["luamake.bin"] or "luamake_lua",
		root = "src/main.cpp",
		compiler = cc,
		install_dir = args["luamake.build"] ~= nil and args["luamake.build"] or "build",
		linking = { "-lstdc++", "-lm" }, -- TODO: have the -lm inherited from the lua project
	})
	b.link_lib(lua, lm)
	return b.install_exe(lm)
end
