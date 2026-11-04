import { useQuery } from '@tanstack/react-query';
import { useState, type FormEvent } from 'react';
import { api, type Realm } from '../api';
import { ConfirmButton, ErrorBox, useAction } from '../ui';

const STATUSES = ['online', 'offline', 'maintenance'];
const EMPTY = { name: '', host: '', port: 7777, adminHost: '', adminPort: '', region: '', status: 'offline', sortOrder: 0 };
type Form = typeof EMPTY;

const toForm = (r: Realm): Form => ({
  name: r.name,
  host: r.host ?? '',
  port: r.port ?? 7777,
  adminHost: r.adminHost ?? '',
  adminPort: r.adminPort ? String(r.adminPort) : '',
  region: r.region ?? '',
  status: r.status ?? 'offline',
  sortOrder: r.sortOrder ?? 0,
});

/** Admin only. Edits go to auth; the admin endpoint is what online, kick and character writes use. */
export function Realms() {
  const realms = useQuery({ queryKey: ['realms'], queryFn: () => api<{ realms: Realm[] }>('/realms') });
  const [editing, setEditing] = useState<number | 'new' | null>(null);
  const [form, setForm] = useState<Form>(EMPTY);
  const save = useAction(
    () => {
      const body = { ...form, adminHost: form.adminHost || null, adminPort: form.adminPort ? Number(form.adminPort) : null };
      return editing === 'new'
        ? api('/realms', { method: 'POST', body })
        : api(`/realms/${editing}`, { method: 'PUT', body });
    },
    [['realms']],
    () => setEditing(null),
  );
  const edit = (target: number | 'new', initial: Form) => {
    save.reset();
    setForm(initial);
    setEditing(target);
  };
  const submit = (e: FormEvent) => {
    e.preventDefault();
    save.mutate(undefined);
  };
  const field = (key: keyof Form, label: string, type = 'text') => (
    <label>
      {label}
      <input
        type={type}
        value={form[key]}
        onChange={(e) => setForm({ ...form, [key]: type === 'number' ? Number(e.target.value) : e.target.value })}
      />
    </label>
  );

  return (
    <section>
      <div className="row between">
        <h1>Realms</h1>
        <button type="button" onClick={() => edit('new', EMPTY)}>
          Add realm
        </button>
      </div>
      <ErrorBox error={realms.error} />
      {editing !== null && (
        <form className="card filters" onSubmit={submit}>
          {field('name', 'Name')}
          {field('host', 'Host')}
          {field('port', 'Port', 'number')}
          {field('adminHost', 'Admin host')}
          {field('adminPort', 'Admin port')}
          {field('region', 'Region')}
          <label>
            Status
            <select value={form.status} onChange={(e) => setForm({ ...form, status: e.target.value })}>
              {STATUSES.map((s) => (
                <option key={s}>{s}</option>
              ))}
            </select>
          </label>
          {field('sortOrder', 'Sort order', 'number')}
          <div className="row">
            <button type="submit" disabled={save.isPending}>
              {editing === 'new' ? 'Create' : 'Save'}
            </button>
            <button type="button" className="secondary" onClick={() => setEditing(null)}>
              Cancel
            </button>
          </div>
          <ErrorBox error={save.error} />
        </form>
      )}
      <table>
        <thead>
          <tr>
            <th>Id</th>
            <th>Name</th>
            <th>Game endpoint</th>
            <th>Admin endpoint</th>
            <th>Region</th>
            <th>Status</th>
            <th>Reachable</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {realms.data?.realms.map((r) => (
            <tr key={r.id}>
              <td>{r.id}</td>
              <td>{r.name}</td>
              <td>
                {r.host}:{r.port}
              </td>
              <td>{r.adminHost ? `${r.adminHost}:${r.adminPort}` : '-'}</td>
              <td>{r.region}</td>
              <td>{r.status}</td>
              <td>
                {r.reachable ? (
                  <span className="badge online">reachable</span>
                ) : (
                  <span className="badge danger">unreachable</span>
                )}
              </td>
              <td>
                <div className="row">
                  <button type="button" className="secondary" onClick={() => edit(r.id, toForm(r))}>
                    Edit
                  </button>
                  <ConfirmButton
                    label="Delete"
                    title={`Delete realm ${r.name}?`}
                    danger
                    run={() => api(`/realms/${r.id}`, { method: 'DELETE' })}
                    invalidate={[['realms']]}
                  >
                    <p>Players no longer see the realm. Its characters stay in the world database.</p>
                  </ConfirmButton>
                </div>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </section>
  );
}
