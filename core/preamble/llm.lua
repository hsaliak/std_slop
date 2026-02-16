function llm_query(query)
  if not query or query == "" then error("llm_query requires a query string") end
  local success, result = call_tool(native.llm_query, {query = query})
  if not success then error("llm_query failed: " .. result) end
  return result
end

function llm_query_async(query)
  if not query or query == "" then error("llm_query_async requires a query string") end
  return tools.dispatch_async("llm_query", {query = query})
end

function tools.llm_query(args)
  return llm_query(args.query)
end

function tools.llm_query_async(args)
  return llm_query_async(args.query)
end

function get_tool_manifest()
  local m = { tools = {} }
  for name, _ in pairs(tools) do
    table.insert(m.tools, name)
  end
  return m
end

