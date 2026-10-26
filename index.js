import { spawn } from 'node:child_process'
import { existsSync, mkdirSync, renameSync, writeFileSync } from 'node:fs'
import { homedir } from 'node:os'
import { basename, dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

export const name = 'dsh-task-pet'
export const inject = ['agents']

const STATE_FILE = process.env.DSH_PET_STATE || join(homedir(), '.dsh', 'dsh-pet', 'state.txt')
const OPEN_URL = process.env.DSH_PET_URL || 'http://127.0.0.1:3080/'
const PET_EXE = fileURLToPath(new URL('./bin/DshPet.exe', import.meta.url))

export function apply(ctx) {
  mkdirSync(dirname(STATE_FILE), { recursive: true })

  const rows = new Map()
  let pending = null
  let lastBody = ''

  const topLevel = (session) => session?.header?.origin !== 'subagent'

  const rowFor = (session) => {
    let row = rows.get(session.id)
    if (!row) {
      row = { todos: [], approvals: 0, at: 0, cwd: session.header?.cwd ?? '' }
      rows.set(session.id, row)
    }
    row.at = Date.now()
    return row
  }

  ctx.on('session/event', (session, event) => {
    if (!topLevel(session)) return
    const row = rowFor(session)
    if (event.type === 'todo/write') {
      row.todos = Array.isArray(event.data?.todos) ? event.data.todos : []
    } else if (event.type === 'approval/asked') {
      row.approvals += 1
    } else if (event.type === 'approval/decided') {
      row.approvals = Math.max(0, row.approvals - 1)
    }
    schedule()
  })

  ctx.on('agent/status', ({ agent }) => {
    if (!topLevel(agent.session)) return
    rowFor(agent.session)
    schedule()
  })

  ctx.on('session/disposed', (session) => {
    rows.delete(session.id)
    schedule()
  })

  const clean = (value) => String(value ?? '').replace(/[\r\n\t]+/g, ' ').trim().slice(0, 300)

  function pick() {
    const live = ctx.agents.list().filter((agent) => topLevel(agent.session))
    let best = null
    for (const agent of live) {
      const row = rows.get(agent.session.id)
      if (!row) continue
      if (!best || row.at > best.row.at) best = { agent, row }
    }
    if (!best && live.length > 0) {
      const agent = live[live.length - 1]
      best = { agent, row: rows.get(agent.session.id) }
    }
    return best
  }

  function render() {
    const best = pick()
    const agent = best?.agent
    const row = best?.row
    const todos = row?.todos ?? []
    const done = todos.filter((todo) => todo.status === 'completed').length
    const active = todos.find((todo) => todo.status === 'in_progress') ?? todos.find((todo) => todo.status === 'pending')
    let status = 'idle'
    if ((row?.approvals ?? 0) > 0) status = 'waiting'
    else if (agent?.status === 'running') status = 'working'
    return [
      `status=${status}`,
      `done=${done}`,
      `total=${todos.length}`,
      `current=${clean(active?.content)}`,
      `title=${clean(row?.cwd ? basename(row.cwd) : 'dsh')}`,
      `session=${agent?.session.id ?? ''}`,
      `url=${clean(OPEN_URL)}`,
      `host=${process.pid}`,
      `updated=${Date.now()}`,
    ].join('\n') + '\n'
  }

  function write() {
    const body = render()
    if (body === lastBody) return
    try {
      writeFileSync(`${STATE_FILE}.tmp`, body)
      renameSync(`${STATE_FILE}.tmp`, STATE_FILE)
      lastBody = body
    } catch {
      // reader or antivirus may hold the file; retry on the next tick
    }
  }

  function schedule() {
    if (pending) return
    pending = setTimeout(() => { pending = null; write() }, 80)
    if (typeof pending.unref === 'function') pending.unref()
  }

  ctx.effect(() => {
    write()

    if (existsSync(PET_EXE)) {
      try {
        const pet = spawn(PET_EXE, [], {
          stdio: 'ignore',
          windowsHide: true,
          env: { ...process.env, DSH_PET_STATE: STATE_FILE },
        })
        pet.on('error', () => {})
        pet.unref()
      } catch {
        // missing or blocked exe only disables the pet, never the harness
      }
    }

    const heartbeat = setInterval(write, 3000)
    if (typeof heartbeat.unref === 'function') heartbeat.unref()

    return () => {
      clearInterval(heartbeat)
      if (pending) clearTimeout(pending)
      try {
        writeFileSync(`${STATE_FILE}.tmp`, lastBody.replace(/^host=.*$/m, 'host=0'))
        renameSync(`${STATE_FILE}.tmp`, STATE_FILE)
      } catch {
        // the pet also exits when its host pid disappears
      }
    }
  })
}
