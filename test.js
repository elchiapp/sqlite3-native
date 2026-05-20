const test = require('brittle')
const { create } = require('./test/helpers')

test('can open a db', async (t) => {
  const sql = create(t)
  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  t.pass('opened the db without throwing')
})

test('can open a db and create many tables', async (t) => {
  const sql = create(t)

  for (let i = 0; i < 10; i++) {
    await sql.exec(
      `CREATE TABLE records${i} (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);`
    )
  }

  t.pass('opened the db without throwing')
})

test('can open a db, insert and select', async (t) => {
  const sql = create(t)
  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await sql.exec("INSERT INTO records (NAME) values ('mathias'), ('andrew');")
  const result = await sql.exec('SELECT ID, NAME FROM records;')
  t.is(result.length, 2)
  t.alike(result[0].columns, ['ID', 'NAME'])
  t.alike(result[0].rows, ['1', 'mathias'])
  t.alike(result[1].rows, ['2', 'andrew'])
})

test('big values', async (t) => {
  const big = Buffer.alloc(4096).fill('big').toString()
  const sql = create(t)
  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await sql.exec("INSERT INTO records (NAME) values ('" + big + "'), ('short');")
  const result = await sql.exec('SELECT ID, NAME FROM records;')
  t.is(result.length, 2)
  t.alike(result[0].columns, ['ID', 'NAME'])
  t.alike(result[0].rows, ['1', big])
  t.alike(result[1].rows, ['2', 'short'])
})

test('basic index', async (t) => {
  const sql = create(t)

  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await sql.exec('CREATE UNIQUE INDEX idx_name ON records (NAME);')
  await sql.exec("INSERT INTO records (NAME) values ('mathias'), ('andrew');")
  const result = await sql.exec("SELECT NAME FROM records WHERE NAME = 'mathias';")
  t.is(result.length, 1)
  t.alike(result[0].columns, ['NAME'])
  t.alike(result[0].rows, ['mathias'])
})

test('bigger index', async (t) => {
  const sql = create(t)

  await sql.exec('CREATE TABLE records (ID INTEGER PRIMARY KEY AUTOINCREMENT, NAME TEXT NOT NULL);')
  await sql.exec('CREATE UNIQUE INDEX idx_name ON records (NAME);')

  for (let i = 0; i < 1000; i++) {
    await sql.exec(`INSERT INTO records (NAME) values ('mr-${i}');`)
  }

  const result = await sql.exec("SELECT NAME FROM records WHERE NAME = 'mr-10';")
  t.is(result.length, 1)
  t.alike(result[0].columns, ['NAME'])
  t.alike(result[0].rows, ['mr-10'])
})

test('query run inserts with params', async (t) => {
  const sql = create(t)

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)',
    [],
    'run'
  )

  const result = await sql.query('INSERT INTO records (name) VALUES (?)', ['qvac'], 'run')
  t.alike(result, { changes: 1, lastInsertRowid: 1 })
})

test('query all selects rows as objects', async (t) => {
  const sql = create(t)

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)',
    [],
    'run'
  )
  await sql.query('INSERT INTO records (name) VALUES (?), (?)', ['ada', 'grace'], 'run')

  const rows = await sql.query('SELECT id, name FROM records ORDER BY id', [], 'all')
  t.alike(rows, [
    { id: 1, name: 'ada' },
    { id: 2, name: 'grace' }
  ])
})

test('query values selects rows as arrays', async (t) => {
  const sql = create(t)

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)',
    [],
    'run'
  )
  await sql.query('INSERT INTO records (name) VALUES (?), (?)', ['ada', 'grace'], 'run')

  const rows = await sql.query('SELECT id, name FROM records ORDER BY id', [], 'values')
  t.alike(rows, [
    [1, 'ada'],
    [2, 'grace']
  ])
})

test('query get returns first row or null', async (t) => {
  const sql = create(t)

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)',
    [],
    'run'
  )
  await sql.query('INSERT INTO records (name) VALUES (?), (?)', ['first', 'second'], 'run')

  const row = await sql.query('SELECT id, name FROM records ORDER BY id', [], 'get')
  t.alike(row, { id: 1, name: 'first' })

  const missing = await sql.query('SELECT id, name FROM records WHERE name = ?', ['missing'], 'get')
  t.is(missing, null)
})

test('query binds params and extracts typed results', async (t) => {
  const sql = create(t)
  const blob = Buffer.from([0, 1, 2, 253, 254, 255])

  const row = await sql.query(
    'SELECT ? AS nil, ? AS integer, ? AS float, ? AS text, ? AS blob',
    [null, 42, 3.25, 'hello', blob],
    'get'
  )

  t.is(row.nil, null)
  t.is(row.integer, 42)
  t.is(row.float, 3.25)
  t.is(row.text, 'hello')
  t.ok(row.blob instanceof Uint8Array)
  t.alike(Buffer.from(row.blob), blob)
})

test('query round-trips blobs', async (t) => {
  const sql = create(t)
  const small = Buffer.from([1, 2, 3, 4])
  const large = Buffer.alloc(64 * 1024)

  for (let i = 0; i < large.length; i++) large[i] = i % 251

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, body BLOB NOT NULL)',
    [],
    'run'
  )
  await sql.query('INSERT INTO records (body) VALUES (?), (?)', [small, large], 'run')

  const rows = await sql.query('SELECT body FROM records ORDER BY id', [], 'values')
  t.alike(Buffer.from(rows[0][0]), small)
  t.alike(Buffer.from(rows[1][0]), large)

  for (let i = 0; i < 25; i++) {
    const body = Buffer.from([i, i + 1, i + 2, i + 3])
    await sql.query('INSERT INTO records (body) VALUES (?)', [body], 'run')
    const row = await sql.query('SELECT body FROM records WHERE id = ?', [i + 3], 'get')
    t.alike(Buffer.from(row.body), body)
  }
})

test('query treats SQL injection content as a param value', async (t) => {
  const sql = create(t)
  const name = "x'); DROP TABLE records; --"

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)',
    [],
    'run'
  )
  await sql.query('INSERT INTO records (name) VALUES (?)', [name], 'run')

  const rows = await sql.query('SELECT name FROM records', [], 'all')
  t.alike(rows, [{ name }])
})

test('query reports errors and remains reusable', async (t) => {
  const sql = create(t)

  await t.exception(sql.query('SELECT * FROM missing_table', [], 'all'), /missing_table/)
  await t.exception(sql.query('SELECT ?', [], 'get'), /bind count mismatch/)
  await t.exception(sql.query('SELECT ?', [1, 2], 'get'), /bind count mismatch/)
  await t.exception(sql.query('SELECT ?', [{}], 'get'), /Unsupported SQLite parameter/)

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)',
    [],
    'run'
  )
  await sql.query('INSERT INTO records (name) VALUES (?)', ['after-error'], 'run')

  const row = await sql.query('SELECT name FROM records', [], 'get')
  t.alike(row, { name: 'after-error' })
})

test('query supports transaction commit and rollback', async (t) => {
  const sql = create(t)

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)',
    [],
    'run'
  )

  await sql.query('BEGIN', [], 'run')
  await sql.query('INSERT INTO records (name) VALUES (?)', ['committed'], 'run')
  await sql.query('COMMIT', [], 'run')

  await sql.query('BEGIN', [], 'run')
  await sql.query('INSERT INTO records (name) VALUES (?)', ['rolled-back'], 'run')
  await sql.query('ROLLBACK', [], 'run')

  const rows = await sql.query('SELECT name FROM records ORDER BY id', [], 'all')
  t.alike(rows, [{ name: 'committed' }])
})

test('query supports unique index lookup and many rows', async (t) => {
  const sql = create(t)

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)',
    [],
    'run'
  )
  await sql.query('CREATE UNIQUE INDEX idx_records_name ON records (name)', [], 'run')

  for (let i = 0; i < 250; i++) {
    await sql.query('INSERT INTO records (name) VALUES (?)', [`record-${i}`], 'run')
  }

  const row = await sql.query('SELECT id, name FROM records WHERE name = ?', ['record-117'], 'get')
  t.alike(row, { id: 118, name: 'record-117' })

  const count = await sql.query('SELECT COUNT(*) AS count FROM records', [], 'get')
  t.alike(count, { count: 250 })
})

test('query returns rows for SQLite PRAGMA statements', async (t) => {
  const sql = create(t)

  await sql.query('CREATE TABLE records (id INTEGER PRIMARY KEY, name TEXT)', [], 'run')

  const columns = await sql.query("PRAGMA table_info('records')", [], 'values')
  t.alike(
    columns.map((row) => row[1]),
    ['id', 'name']
  )

  const options = await sql.query('PRAGMA compile_options', [], 'values')
  t.ok(options.length > 0)
})

test('query close waits for pending work and rejects later work', async (t) => {
  const sql = create(t)

  await sql.query(
    'CREATE TABLE records (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL)',
    [],
    'run'
  )

  const pending = sql.query('INSERT INTO records (name) VALUES (?)', ['pending'], 'run')
  const closing = sql.close()

  const result = await pending
  await closing

  t.is(result.changes, 1)
  await t.exception(sql.query('SELECT 1', [], 'get'), /closed/)

  const completed = create(t)
  await completed.query('SELECT 1', [], 'get')
  await completed.close()
})
