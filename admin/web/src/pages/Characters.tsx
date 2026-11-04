import { keepPreviousData, useQuery } from '@tanstack/react-query';
import { useState } from 'react';
import { Link, useParams } from 'react-router-dom';
import { api, ApiError, isCode, type Character, type CharacterDetail as Detail, type OnlineEntry } from '../api';
import { useIsAdmin } from '../App';
import { ConfirmButton, Dialog, ErrorBox, fmtDate, Pager, useAction } from '../ui';

const LIMIT = 25;

export function CharacterList() {
  const [filter, setFilter] = useState({ realmId: '', accountId: '', name: '', includeDeleted: false });
  const [offset, setOffset] = useState(0);
  const set = (patch: Partial<typeof filter>) => {
    setFilter({ ...filter, ...patch });
    setOffset(0);
  };
  const list = useQuery({
    queryKey: ['characters', filter, offset],
    queryFn: () =>
      api<{ characters: Character[]; total: number }>('/characters', { query: { ...filter, limit: LIMIT, offset } }),
    placeholderData: keepPreviousData,
  });

  return (
    <section>
      <h1>Characters</h1>
      <div className="card filters">
        <label>
          Name starts with
          <input value={filter.name} onChange={(e) => set({ name: e.target.value })} />
        </label>
        <label>
          Realm id
          <input inputMode="numeric" value={filter.realmId} onChange={(e) => set({ realmId: e.target.value })} />
        </label>
        <label>
          Account id
          <input inputMode="numeric" value={filter.accountId} onChange={(e) => set({ accountId: e.target.value })} />
        </label>
        <label className="check">
          <input type="checkbox" checked={filter.includeDeleted} onChange={(e) => set({ includeDeleted: e.target.checked })} />
          Include deleted
        </label>
      </div>
      <ErrorBox error={list.error} />
      {list.data && (
        <>
          <table>
            <thead>
              <tr>
                <th>Id</th>
                <th>Name</th>
                <th>Realm</th>
                <th>Account</th>
                <th>Map</th>
                <th>Last login</th>
                <th>Deleted</th>
              </tr>
            </thead>
            <tbody>
              {list.data.characters.map((c) => (
                <tr key={c.id} className={c.deletedAt ? 'deleted' : ''}>
                  <td>{c.id}</td>
                  <td>
                    <Link to={`/characters/${c.id}`}>{c.name}</Link>
                  </td>
                  <td>{c.realmId}</td>
                  <td>
                    <Link to={`/accounts/${c.accountId}`}>{c.accountId}</Link>
                  </td>
                  <td>{c.mapId}</td>
                  <td>{fmtDate(c.lastLoginAt)}</td>
                  <td>{fmtDate(c.deletedAt)}</td>
                </tr>
              ))}
            </tbody>
          </table>
          <Pager total={list.data.total} limit={LIMIT} offset={offset} onChange={setOffset} />
        </>
      )}
    </section>
  );
}

export function CharacterDetail() {
  const id = Number(useParams().id);
  const character = useQuery({ queryKey: ['character', id], queryFn: () => api<Detail>(`/characters/${id}`) });
  if (character.isError) return <ErrorBox error={character.error} />;
  return character.data ? <CharacterView c={character.data} /> : null;
}

function CharacterView({ c }: { c: Detail }) {
  const admin = useIsAdmin();
  const onlineKey = ['online', c.realmId];
  const online = useQuery({
    queryKey: onlineKey,
    queryFn: () => api<{ online: OnlineEntry[] }>(`/realms/${c.realmId}/online`),
  });
  const session = online.data?.online.find((o) => o.characterId === c.id);
  const invalidate = [['character', c.id], ['characters'], onlineKey];

  const [force, setForce] = useState(false);
  const [name, setName] = useState(c.name);
  const [pos, setPos] = useState({ mapId: c.mapId, x: c.position.x, y: c.position.y, z: c.position.z, heading: c.heading });
  const [components, setComponents] = useState(JSON.stringify(c.components, null, 2));
  const [options, setOptions] = useState(JSON.stringify(c.options, null, 2));
  const [jsonError, setJsonError] = useState<ApiError | null>(null);
  const [conflictOpen, setConflictOpen] = useState(false);
  const [kickReason, setKickReason] = useState('');

  const write = useAction(
    (req: { method: 'PUT' | 'POST' | 'DELETE'; path: string; body?: Record<string, unknown> }) =>
      api<Detail>(`/characters/${c.id}${req.path}`, { method: req.method, body: { ...req.body, ...(force && { force }) } }),
    invalidate,
  );
  const send = (req: Parameters<typeof write.mutate>[0]) =>
    write.mutate(req, { onError: (err) => isCode(err, 'character_online') && setConflictOpen(true) });
  const sendJson = (field: 'components' | 'options', text: string) => {
    let value: unknown;
    try {
      value = JSON.parse(text);
    } catch (err) {
      write.reset();
      return setJsonError(new ApiError(0, 'invalid_json', `${field}: ${(err as Error).message}`));
    }
    setJsonError(null);
    send({ method: 'PUT', path: `/${field}`, body: { [field]: value } });
  };
  const kick = useAction(
    (reason: string) => api(`/realms/${c.realmId}/kick`, { method: 'POST', body: { characterId: c.id, reason } }),
    [onlineKey],
    () => {
      setConflictOpen(false);
      write.reset();
    },
  );
  // Admins may write through an unreachable realm; the audit marks it forced.
  const unreachable = isCode(online.error, 'realm_unreachable') || isCode(write.error, 'realm_unreachable');

  return (
    <section>
      <h1>
        {c.name} <span className="muted">character {c.id}</span>{' '}
        {c.deletedAt && <span className="badge danger">deleted</span>}
        {session && <span className="badge online">online</span>}
      </h1>
      <div className="grid2">
        <div className="card">
          <h2>Info</h2>
          <dl>
            <dt>Realm</dt>
            <dd>{c.realmId}</dd>
            <dt>Account</dt>
            <dd>
              <Link to={`/accounts/${c.accountId}`}>{c.accountId}</Link>
            </dd>
            <dt>Map</dt>
            <dd>{c.mapId}</dd>
            <dt>Position</dt>
            <dd>
              {c.position.x}, {c.position.y}, {c.position.z} (heading {c.heading})
            </dd>
            <dt>Created</dt>
            <dd>{fmtDate(c.createdAt)}</dd>
            <dt>Last login</dt>
            <dd>{fmtDate(c.lastLoginAt)}</dd>
            <dt>Deleted</dt>
            <dd>{fmtDate(c.deletedAt)}</dd>
            <dt>Status</dt>
            <dd>
              {online.isPending ? 'checking...' : online.isError ? 'unknown' : session ? 'online' : 'offline'}
            </dd>
          </dl>
          <ErrorBox error={online.error} />
          {session && (
            <ConfirmButton
              label="Kick"
              title={`Kick ${c.name}?`}
              danger
              reason
              run={(reason) => api(`/realms/${c.realmId}/kick`, { method: 'POST', body: { characterId: c.id, reason } })}
              invalidate={[onlineKey]}
            />
          )}
        </div>

        <div className="card">
          <h2>Edit</h2>
          <ErrorBox error={jsonError ?? (conflictOpen ? null : write.error)} />
          {write.isSuccess && <p className="notice">Saved.</p>}
          {admin && unreachable && (
            <label className="check warn">
              <input type="checkbox" checked={force} onChange={(e) => setForce(e.target.checked)} />
              Force: write without the online check (audited as forced)
            </label>
          )}
          <div className="row">
            <input value={name} onChange={(e) => setName(e.target.value)} maxLength={16} aria-label="Name" />
            <button type="button" onClick={() => send({ method: 'PUT', path: '/name', body: { name } })}>
              Rename
            </button>
          </div>
          <div className="row wrap">
            {(['mapId', 'x', 'y', 'z', 'heading'] as const).map((k) => (
              <label key={k} className="small">
                {k}
                <input
                  type="number"
                  step="any"
                  value={pos[k]}
                  onChange={(e) => setPos({ ...pos, [k]: Number(e.target.value) })}
                />
              </label>
            ))}
            <button type="button" onClick={() => send({ method: 'PUT', path: '/position', body: pos })}>
              Move
            </button>
          </div>
          <label>
            Components (JSON object)
            <textarea rows={8} value={components} onChange={(e) => setComponents(e.target.value)} />
          </label>
          <button type="button" onClick={() => sendJson('components', components)}>
            Save components
          </button>
          <label>
            Options (JSON object)
            <textarea rows={4} value={options} onChange={(e) => setOptions(e.target.value)} />
          </label>
          <button type="button" onClick={() => sendJson('options', options)}>
            Save options
          </button>
          <div className="row">
            {c.deletedAt ? (
              <button type="button" onClick={() => send({ method: 'POST', path: '/restore' })}>
                Restore
              </button>
            ) : (
              <ConfirmButton
                label="Delete character"
                title={`Delete ${c.name}?`}
                danger
                run={() => api<Detail>(`/characters/${c.id}`, { method: 'DELETE', body: force ? { force } : {} })}
                invalidate={invalidate}
              >
                <p>The character is soft-deleted and can be restored later. Its name stays reserved.</p>
              </ConfirmButton>
            )}
          </div>
        </div>
      </div>

      {conflictOpen && (
        <Dialog
          title={`${c.name} is online`}
          confirmLabel="Kick"
          danger
          disabled={kick.isPending || !kickReason.trim()}
          error={kick.error}
          onConfirm={() => kick.mutate(kickReason.trim())}
          onCancel={() => setConflictOpen(false)}
        >
          <ErrorBox error={write.error} />
          <p>The character must be offline before it can be changed. Kick it, then try again.</p>
          <label>
            Reason
            <input autoFocus value={kickReason} maxLength={200} onChange={(e) => setKickReason(e.target.value)} />
          </label>
        </Dialog>
      )}
    </section>
  );
}
