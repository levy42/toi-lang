local function bench(name, fn)
  local start = os.clock()
  local result = fn()
  local elapsed = os.clock() - start
  print(string.format("%s %.6f sec", name, elapsed))
  return result
end

local function bench_math()
  local acc = 0
  for i = 1, 20000000 do
    acc = acc + (i * 3.14159) / ((i % 97) + 1)
  end
  return acc
end

local function bench_random()
  local acc = 0
  for i = 1, 4000000 do
    acc = acc + math.random()
  end
  return acc
end

local function bench_string_concat()
  local total = 0
  for pass = 1, 200 do
    local parts = {}
    for i = 1, 20000 do
      parts[i] = "abc"
    end
    local s = table.concat(parts, "")
    total = total + #s
  end
  return total
end

local function bench_string_ops()
  local total = 0
  for i = 1, 140000 do
    local s = "the quick brown fox jumps over the lazy dog " .. tostring(i)
    local x = string.upper(s)
    local y = string.lower(x)
    local z = string.sub(y, 5, 19)
    total = total + #z
  end
  return total
end

local function bench_table()
  local t = {0}
  for i = 1, 200000 do
    t[i] = i * 2
  end
  local total = 0
  for pass = 1, 50 do
    for i = 1, 200000 do
      total = total + t[i]
    end
  end
  return total
end

local function bench_sort()
  local total = 0
  for pass = 1, 6 do
    local values = {}
    for i = 1, 120000 do
      values[i] = (i * 1103515245 + 12345) % 1000003
    end
    table.sort(values)
    for i = 1, 120000 do
      total = total + values[i]
    end
  end
  return total
end

local function bench_list_comprehension()
  local t = {}
  for i = 1, 200000 do
    t[i] = i * 3
  end
  local total = 0
  for pass = 1, 55 do
    for i = 1, 200000 do
      total = total + t[i]
    end
  end
  return total
end

local function bench_calls()
  local function add(a, b)
    return a
  end
  local acc = 0
  for i = 1, 6000000 do
    acc = add(acc, "df")
  end
  return acc
end

local function bench_fstring()
  local total = 0
  for i = 1, 600000 do
    local s = string.format("user:%d-x%d", i, i % 10)
    total = total + #s
  end
  return total
end

local function bench_app_pipeline()
  local methods = {"GET", "POST", "PUT", "DELETE"}
  local paths = {"/users", "/teams", "/search", "/assets", "/health", "/events"}

  local routes = {
    ["GET /users"] = 1,
    ["POST /users"] = 2,
    ["GET /teams"] = 3,
    ["PUT /teams"] = 4,
    ["GET /search"] = 5,
    ["GET /assets"] = 6,
    ["GET /health"] = 7,
    ["POST /events"] = 8,
  }

  local cache = {}
  local total = 0
  for i = 1, 600000 do
    local method = methods[(i % 4) + 1]
    local path = paths[(i % 6) + 1]
    local key = method .. " " .. path

    local route = routes[key] or 0
    local auth = (i % 3) ~= 0
    local user_id = i % 10000
    local team_id = i % 100
    local body_size = (i * 37) % 2048

    local status = 200
    if (not auth) and path ~= "/health" then
      status = 401
    elseif method == "POST" and body_size < 32 then
      status = 422
    elseif route == 0 then
      status = 404
    end

    local cache_key = path .. ":" .. (user_id % 32) .. ":" .. team_id
    local cached = cache[cache_key]
    if cached ~= nil then
      total = total + cached
    else
      local cache_value = route + status + body_size
      cache[cache_key] = cache_value
      total = total + cache_value
    end

    total = total + status + route + user_id + team_id
  end

  return total
end

local function bench_app_handlers()
  local methods = {"GET", "GET", "GET", "POST", "GET", "PUT", "DELETE", "GET"}
  local paths = {"/users", "/teams", "/users", "/events", "/health", "/teams", "/assets", "/users"}

  local function h_users(route, status, body_size, user_id, team_id)
    return route + status + body_size + (user_id % 7) + (team_id % 5)
  end

  local function h_teams(route, status, body_size, user_id, team_id)
    return route + status + (body_size % 97) + (user_id % 11) + (team_id % 3)
  end

  local function h_events(route, status, body_size, user_id, team_id)
    return route + status + body_size + (user_id % 13) + (team_id % 17)
  end

  local function h_health(route, status, body_size, user_id, team_id)
    return route + status + (body_size % 17) + (user_id % 5) + (team_id % 2)
  end

  local function h_fallback(route, status, body_size, user_id, team_id)
    return route + status + body_size + user_id + team_id
  end

  local handlers = {
    ["GET /users"] = h_users,
    ["GET /teams"] = h_teams,
    ["POST /events"] = h_events,
    ["GET /health"] = h_health,
  }

  local routes = {
    ["GET /users"] = 1,
    ["GET /teams"] = 3,
    ["POST /events"] = 8,
    ["GET /health"] = 7,
  }

  local total = 0
  for i = 1, 600000 do
    local method = methods[(i % 8) + 1]
    local path = paths[(i % 8) + 1]
    local key = method .. " " .. path
    local route = routes[key] or 0

    local auth = (i % 3) ~= 0
    local body_size = (i * 37) % 2048
    local user_id = i % 10000
    local team_id = i % 100

    local status = 200
    if (not auth) and path ~= "/health" then
      status = 401
    elseif method == "POST" and body_size < 32 then
      status = 422
    elseif route == 0 then
      status = 404
    end

    local handler = handlers[key] or h_fallback
    total = total + handler(route, status, body_size, user_id, team_id)
  end

  return total
end

local function bench_memory()
  collectgarbage("collect")
  local base = collectgarbage("count") * 1024
  local t = {}
  for i = 1, 500000 do
    t[i] = string.format("abc%d", i)
  end
  collectgarbage("collect")
  local used = (collectgarbage("count") * 1024) - base
  return used
end

print("Lua perf:")
bench("math", bench_math)
bench("random", bench_random)
bench("string concat", bench_string_concat)
bench("string ops", bench_string_ops)
bench("table", bench_table)
bench("sort", bench_sort)
bench("list comprehension", bench_list_comprehension)
bench("calls", bench_calls)
bench("f-string", bench_fstring)
bench("app pipeline", bench_app_pipeline)
bench("app handlers", bench_app_handlers)
local used = bench("memory", bench_memory)
print(string.format("memory %d bytes", used))
