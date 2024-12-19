function Dump(o)
	if type(o) == "table" then
		local s = "{"
		for k, v in pairs(o) do
			if type(k) ~= "number" then
				k = '"' .. k .. '"'
			end
			s = s .. "[" .. k .. "]=" .. Dump(v) .. ","
		end
		return s .. "}"
	elseif type(o) == "string" then
		return '"' .. o .. '"'
	else
		return tostring(o)
	end
end
