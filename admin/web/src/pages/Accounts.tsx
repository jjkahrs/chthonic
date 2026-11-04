import { keepPreviousData, useQuery } from '@tanstack/react-query';
import { useState, type FormEvent } from 'react';
import { Link, useNavigate, useParams } from 'react-router-dom';
import { api, type Account, type Character, type Kicks, type Role } from '../api';
import { useCaller, useIsAdmin } from '../App';
import { ConfirmButton, ErrorBox, fmtDate, KickSummary, Pager, useAction, yesNo } from '../ui';

const LIMIT = 25;
const ROLES: Role[] = ['player', 'gm', 'admin'];

function Tri(props: { label: string; value: string; onChange: (v: string) => void }) {
  return (
    <label>
      {props.label}
      <select value={props.value} onChange={(e) => props.onChange(e.target.value)}>
        <option value="">any</option>
        <option value="true">yes</option>
        <option value="false">no</option>
      </select>
    </label>
  );
}

export function AccountList() {
  const admin = useIsAdmin();
  const [filter, setFilter] = useState({ q: '', role: '', banned: '', verified: '', locked: '' });
  const [offset, setOffset] = useState(0);
  const set = (key: keyof typeof filter) => (value: string) => {
    setFilter({ ...filter, [key]: value });
    setOffset(0);
  };
  const list = useQuery({
    queryKey: ['accounts', filter, offset],
    queryFn: () =>
      api<{ accounts: Account[]; total: number }>('/accounts', { query: { ...filter, limit: LIMIT, offset } }),
    placeholderData: keepPreviousData,
  });

  return (
    <section>
      <div className="row between">
        <h1>Accounts</h1>
        {admin && (
          <Link className="button" to="/accounts/new">
            Create account
          </Link>
        )}
      </div>
      <div className="card filters">
        <label>
          Email or id
          <input value={filter.q} onChange={(e) => set('q')(e.target.value)} placeholder="search" />
        </label>
        <label>
          Role
          <select value={filter.role} onChange={(e) => set('role')(e.target.value)}>
            <option value="">any</option>
            {ROLES.map((r) => (
              <option key={r}>{r}</option>
            ))}
          </select>
        </label>
        <Tri label="Banned" value={filter.banned} onChange={set('banned')} />
        <Tri label="Verified" value={filter.verified} onChange={set('verified')} />
        <Tri label="Locked" value={filter.locked} onChange={set('locked')} />
      </div>
      <ErrorBox error={list.error} />
      {list.data && (
        <>
          <table>
            <thead>
              <tr>
                <th>Id</th>
                <th>Email</th>
                <th>Role</th>
                <th>Verified</th>
                <th>Locked until</th>
                <th>Banned</th>
                <th>Created</th>
              </tr>
            </thead>
            <tbody>
              {list.data.accounts.map((a) => (
                <tr key={a.id}>
                  <td>{a.id}</td>
                  <td>
                    <Link to={`/accounts/${a.id}`}>{a.email}</Link>
                  </td>
                  <td>{a.role}</td>
                  <td>{yesNo(a.emailVerified)}</td>
                  <td>{fmtDate(a.lockedUntil)}</td>
                  <td>{a.activeBan ? <span className="badge danger">banned</span> : 'no'}</td>
                  <td>{fmtDate(a.createdAt)}</td>
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

export function CreateAccount() {
  const navigate = useNavigate();
  const [form, setForm] = useState({ email: '', password: '', role: 'player' as Role });
  const create = useAction(
    () => api<Account>('/accounts', { method: 'POST', body: form }),
    [['accounts']],
    (a) => navigate(`/accounts/${a.id}`),
  );
  const submit = (e: FormEvent) => {
    e.preventDefault();
    create.mutate(undefined);
  };
  return (
    <section>
      <h1>Create account</h1>
      <form className="card narrow" onSubmit={submit}>
        <p className="muted">The account is created with a verified email.</p>
        <label>
          Email
          <input type="email" required value={form.email} onChange={(e) => setForm({ ...form, email: e.target.value })} />
        </label>
        <label>
          Password (10-128 characters)
          <input
            type="password"
            autoComplete="new-password"
            required
            minLength={10}
            maxLength={128}
            value={form.password}
            onChange={(e) => setForm({ ...form, password: e.target.value })}
          />
        </label>
        <label>
          Role
          <select value={form.role} onChange={(e) => setForm({ ...form, role: e.target.value as Role })}>
            {ROLES.map((r) => (
              <option key={r}>{r}</option>
            ))}
          </select>
        </label>
        <ErrorBox error={create.error} />
        <button type="submit" disabled={create.isPending}>
          Create
        </button>
      </form>
    </section>
  );
}

export function AccountDetail() {
  const id = Number(useParams().id);
  const caller = useCaller();
  const admin = caller.role === 'admin';
  const navigate = useNavigate();
  const [kicks, setKicks] = useState<Kicks | null>(null);
  const [expires, setExpires] = useState('');
  const key = ['account', id];
  const account = useQuery({ queryKey: key, queryFn: () => api<Account>(`/accounts/${id}`) });
  const characters = useQuery({
    queryKey: ['characters', { accountId: id, includeDeleted: true }],
    queryFn: () => api<{ characters: Character[] }>('/characters', { query: { accountId: id, includeDeleted: true, limit: 100 } }),
  });
  const [role, setRole] = useState<Role | ''>('');
  const post = (path: string) => api(`/accounts/${id}/${path}`, { method: 'POST' });
  const simple = useAction(post, [key]);
  const unban = useAction(() => api(`/accounts/${id}/ban`, { method: 'DELETE' }), [key, ['accounts']]);
  const setRoleAction = useAction(
    (r: Role) => api<{ kicks: Kicks | null }>(`/accounts/${id}/role`, { method: 'PUT', body: { role: r } }),
    [key, ['accounts']],
    (res) => setKicks(res.kicks),
  );

  if (account.isError) return <ErrorBox error={account.error} />;
  const a = account.data;
  if (!a) return null;
  const self = a.id === caller.id;
  // GMs moderate players only; nobody moderates themselves.
  const canBan = !self && (admin || a.role === 'player');

  return (
    <section>
      <h1>
        Account {a.id} <span className="muted">{a.email}</span>
      </h1>
      <div className="grid2">
        <div className="card">
          <h2>Info</h2>
          <dl>
            <dt>Email</dt>
            <dd>{a.email}</dd>
            <dt>Role</dt>
            <dd>{a.role}</dd>
            <dt>Email verified</dt>
            <dd>{a.emailVerified ? fmtDate(a.emailVerifiedAt) : 'no'}</dd>
            <dt>Failed logins</dt>
            <dd>{a.failedLoginCount}</dd>
            <dt>Locked until</dt>
            <dd>{fmtDate(a.lockedUntil)}</dd>
            <dt>Created</dt>
            <dd>{fmtDate(a.createdAt)}</dd>
            <dt>Ban</dt>
            <dd>
              {a.activeBan ? (
                <>
                  <span className="badge danger">banned</span> {a.activeBan.reason} (until {fmtDate(a.activeBan.expiresAt)})
                </>
              ) : (
                'none'
              )}
            </dd>
          </dl>
        </div>
        <div className="card">
          <h2>Actions</h2>
          <KickSummary kicks={kicks} />
          <ErrorBox error={simple.error ?? unban.error ?? setRoleAction.error} />
          {simple.isSuccess && <p className="notice">Done.</p>}
          <div className="row wrap">
            {canBan && !a.activeBan && (
              <ConfirmButton
                label="Ban"
                title={`Ban ${a.email}?`}
                danger
                reason
                run={(reason) =>
                  api<{ kicks: Kicks }>(`/accounts/${id}/ban`, {
                    method: 'POST',
                    body: { reason, expiresAt: expires ? new Date(expires).toISOString() : null },
                  })
                }
                invalidate={[key, ['accounts']]}
                onDone={(res) => setKicks(res.kicks)}
              >
                <label>
                  Expires (empty = permanent)
                  <input type="datetime-local" value={expires} onChange={(e) => setExpires(e.target.value)} />
                </label>
                <p className="muted">Online characters on every realm are kicked.</p>
              </ConfirmButton>
            )}
            {canBan && a.activeBan && (
              <button type="button" onClick={() => unban.mutate(undefined)}>
                Unban
              </button>
            )}
            {admin && !a.emailVerified && (
              <button type="button" onClick={() => simple.mutate('verify-email')}>
                Verify email
              </button>
            )}
            {admin && (
              <button type="button" onClick={() => simple.mutate('password-reset')}>
                Send password reset
              </button>
            )}
            {admin && (a.lockedUntil || a.failedLoginCount > 0) && (
              <button type="button" onClick={() => simple.mutate('unlock')}>
                Unlock
              </button>
            )}
            {admin && (
              <button type="button" onClick={() => simple.mutate('revoke-sessions')}>
                Revoke sessions
              </button>
            )}
          </div>
          {admin && !self && (
            <div className="row">
              <select value={role || a.role} onChange={(e) => setRole(e.target.value as Role)}>
                {ROLES.map((r) => (
                  <option key={r}>{r}</option>
                ))}
              </select>
              <button type="button" disabled={!role || role === a.role} onClick={() => role && setRoleAction.mutate(role)}>
                Set role
              </button>
            </div>
          )}
          {admin && !self && (
            <div className="row">
              <ConfirmButton
                label="Delete account"
                title={`Delete ${a.email}?`}
                danger
                run={() => api(`/accounts/${id}`, { method: 'DELETE' })}
                invalidate={[['accounts'], ['characters']]}
                onDone={() => navigate('/accounts')}
              >
                <p>
                  This kicks the account's online characters, deletes the account in auth and soft-deletes all its
                  characters. It cannot be undone.
                </p>
              </ConfirmButton>
            </div>
          )}
        </div>
      </div>

      <div className="card">
        <h2>Characters</h2>
        <ErrorBox error={characters.error} />
        <table>
          <thead>
            <tr>
              <th>Id</th>
              <th>Realm</th>
              <th>Name</th>
              <th>Map</th>
              <th>Last login</th>
              <th>Deleted</th>
            </tr>
          </thead>
          <tbody>
            {characters.data?.characters.map((c) => (
              <tr key={c.id}>
                <td>{c.id}</td>
                <td>{c.realmId}</td>
                <td>
                  <Link to={`/characters/${c.id}`}>{c.name}</Link>
                </td>
                <td>{c.mapId}</td>
                <td>{fmtDate(c.lastLoginAt)}</td>
                <td>{fmtDate(c.deletedAt)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      <div className="card">
        <h2>Ban history</h2>
        <table>
          <thead>
            <tr>
              <th>Id</th>
              <th>Reason</th>
              <th>Banned</th>
              <th>By</th>
              <th>Expires</th>
              <th>Lifted</th>
              <th>Lifted by</th>
            </tr>
          </thead>
          <tbody>
            {a.bans?.map((b) => (
              <tr key={b.id}>
                <td>{b.id}</td>
                <td>{b.reason}</td>
                <td>{fmtDate(b.createdAt)}</td>
                <td>{b.bannedBy ?? '-'}</td>
                <td>{fmtDate(b.expiresAt)}</td>
                <td>{fmtDate(b.liftedAt)}</td>
                <td>{b.liftedBy ?? '-'}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}
