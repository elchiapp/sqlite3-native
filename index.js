const ReadyResource = require('ready-resource')
const binding = require('./binding')
const VFS = require('./lib/vfs')
const MemoryVFS = require('./lib/memory-vfs')
const constants = require('./lib/constants')

module.exports = exports = class SQLite3 extends ReadyResource {
  constructor(opts = {}) {
    const { name = 'sqlite3.db', vfs = new MemoryVFS(), extensions = false } = opts

    super()

    this._name = name
    this._vfs = vfs
    this._extensions = extensions

    this._handle = binding.init(this)
    this._queue = Promise.resolve()
  }

  get name() {
    return this._name
  }

  async exec(query) {
    const ready = this._readyForQuery()
    if (ready) await ready

    return this._enqueue(() => binding.exec(this._handle, query))
  }

  async query(sql, params = [], mode = 'all') {
    const ready = this._readyForQuery()
    if (ready) await ready

    return this._enqueue(() => binding.query(this._handle, sql, params, mode))
  }

  _enqueue(operation) {
    const run = () => {
      try {
        return operation()
      } catch (err) {
        return Promise.reject(err)
      }
    }
    const next = this._queue.then(run, run)
    this._queue = next.catch(() => {})
    return next
  }

  _readyForQuery() {
    if (this.closed || this.closing) throw new Error('SQLite database is closed')
    if (this.opened === true) return null

    return this.ready().then(() => {
      if (this.closed || this.closing) throw new Error('SQLite database is closed')
    })
  }

  async loadExtension(path, entry = null) {
    if (this._extensions === false) throw new Error('Extension loading is disabled')

    if (this.opened === false) await this.ready()

    return binding.loadExtension(this._handle, path, entry)
  }

  async _open() {
    await binding.open(this._handle, this._vfs._handle, this._name, this._extensions)
  }

  async _close() {
    await this._queue

    if (this.opened) await binding.close(this._handle)

    this._vfs.destroy()
  }
}

exports.VFS = VFS
exports.MemoryVFS = MemoryVFS
exports.constants = constants
