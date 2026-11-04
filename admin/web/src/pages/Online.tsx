import { useQuery } from '@tanstack/react-query';
import { useState } from 'react';
import { Link } from 'react-router-dom';
import { api, type OnlineEntry, type Realm } from '../api';
import { ConfirmButton, ErrorBox, fmtDate } from '../ui';

export function Online() {
  const realms = useQuery({ queryKey: ['realms'], queryFn: () => api<{ realms: Realm[] }>('/realms') });
  const [picked, setPicked] = useState<number | null>(null);
  const list = realms.data?.realms;
  const realmId = picked ?? (list?.find((r) => r.reachable) ?? list?.[0])?.id;
  const online = useQuery({
    queryKey: ['online', realmId],
    queryFn: () => api<{ online: OnlineEntry[] }>(`/realms/${realmId}/online`),
    enabled: realmId !== undefined,
  });

  return (
    <section>
      <h1>Online</h1>
      <ErrorBox error={realms.error} />
      <div className="tabs">
        {realms.data?.realms.map((r) => (
          <button
            type="button"
            key={r.id}
            className={r.id === realmId ? 'tab active' : 'tab'}
            onClick={() => setPicked(r.id)}
          >
            {r.name} <span className={r.reachable ? 'dot ok' : 'dot down'} title={r.reachable ? 'reachable' : 'unreachable'} />
          </button>
        ))}
        {realms.data?.realms.length === 0 && <p className="muted">No realms.</p>}
      </div>
      <ErrorBox error={online.error} />
      {online.data && (
        <table>
          <thead>
            <tr>
              <th>Character</th>
              <th>Account</th>
              <th>Map</th>
              <th>Session</th>
              <th>Connected</th>
              <th />
            </tr>
          </thead>
          <tbody>
            {online.data.online.map((o) => (
              <tr key={o.sessionId}>
                <td>
                  {o.characterId ? <Link to={`/characters/${o.characterId}`}>{o.characterName}</Link> : <em>lobby</em>}
                </td>
                <td>
                  <Link to={`/accounts/${o.accountId}`}>{o.accountId}</Link>
                </td>
                <td>{o.mapId}</td>
                <td>{o.sessionId}</td>
                <td>{fmtDate(o.connectedAt)}</td>
                <td>
                  <ConfirmButton
                    label="Kick"
                    title={`Kick ${o.characterName || `account ${o.accountId}`}?`}
                    danger
                    reason
                    run={(reason) => api(`/realms/${realmId}/kick`, { method: 'POST', body: { accountId: o.accountId, reason } })}
                    invalidate={[['online', realmId]]}
                  />
                </td>
              </tr>
            ))}
            {online.data.online.length === 0 && (
              <tr>
                <td colSpan={6} className="muted">
                  Nobody is online.
                </td>
              </tr>
            )}
          </tbody>
        </table>
      )}
    </section>
  );
}
