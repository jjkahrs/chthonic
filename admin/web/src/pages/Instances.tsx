import { useQuery } from '@tanstack/react-query';
import { useState } from 'react';
import { api, type InstanceEntry, type Realm } from '../api';
import { ErrorBox, fmtDate } from '../ui';

/**
 * A realm's live map contexts (docs/world/instances.html): each world map's one context (instance 0) and every
 * instance copy, with its key, players, NPCs and, while empty, when its timer started. Read-only; Refresh re-asks.
 */
export function Instances() {
  const realms = useQuery({ queryKey: ['realms'], queryFn: () => api<{ realms: Realm[] }>('/realms') });
  const [picked, setPicked] = useState<number | null>(null);
  const list = realms.data?.realms;
  const realmId = picked ?? (list?.find((r) => r.reachable) ?? list?.[0])?.id;
  const instances = useQuery({
    queryKey: ['instances', realmId],
    queryFn: () => api<{ instances: InstanceEntry[] }>(`/realms/${realmId}/instances`),
    enabled: realmId !== undefined,
  });
  const copies = instances.data?.instances.filter((i) => i.instanceId !== 0).length ?? 0;

  return (
    <section>
      <div className="row between">
        <h1>Instances</h1>
        <button type="button" onClick={() => instances.refetch()} disabled={realmId === undefined || instances.isFetching}>
          Refresh
        </button>
      </div>
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
      <ErrorBox error={instances.error} />
      {instances.data && (
        <>
          <p className="muted">
            {instances.data.instances.length - copies} world map contexts, {copies} instance copies
          </p>
          <table>
            <thead>
              <tr>
                <th>Map</th>
                <th>Name</th>
                <th>Instance</th>
                <th>Key</th>
                <th>Players</th>
                <th>NPCs</th>
                <th>Created</th>
                <th>Empty since</th>
              </tr>
            </thead>
            <tbody>
              {instances.data.instances.map((i) => (
                <tr key={`${i.mapId}/${i.instanceId}`}>
                  <td>{i.mapId}</td>
                  <td>{i.mapName}</td>
                  <td>{i.instanceId ? i.instanceId : <span className="badge">world</span>}</td>
                  <td>{i.key || <span className="muted">{i.instanceId ? 'none' : '-'}</span>}</td>
                  <td>{i.players}</td>
                  <td>{i.npcs}</td>
                  <td>{fmtDate(i.createdAt)}</td>
                  <td>{i.emptySince ? <span className="badge danger">{fmtDate(i.emptySince)}</span> : '-'}</td>
                </tr>
              ))}
              {instances.data.instances.length === 0 && (
                <tr>
                  <td colSpan={8} className="muted">
                    No map contexts are running.
                  </td>
                </tr>
              )}
            </tbody>
          </table>
        </>
      )}
    </section>
  );
}
