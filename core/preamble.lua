-- Slop Lua Preamble
function llm_query(query)
  if not query or query == "" then error("llm_query requires a query string") end
  local escaped = query:gsub("'", "'\\''")
  local success, result = tools.execute_bash({command = "std_slop --prompt '" .. escaped .. "'"})
  if not success then error("llm_query failed: " .. result) end
  return result
end

-- Also available in the tools table for consistency with C++ tools
tools = tools or {}
tools.llm_query = function(args)
  return llm_query(args.query)
end

manifest = { tools = {} }
for name, _ in pairs(tools) do
  table.insert(manifest.tools, name)
end
