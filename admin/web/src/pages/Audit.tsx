import { keepPreviousData, useQuery } from '@tanstack/react-query';
import { useState } from 'react';
import { api, type AuditEntry } from '../api';
import { ErrorBox, fmtDate, Pager } from '../ui';

const LIMIT = 50;
const iso = (local: string) => (local ? new Date(local).toISOString() : undefined);

export function Audit() {
  const [filter, setFilter] = useState({ actorId: '', action: '', targetType: '', targetId: '', from: '', to: '' });
  const [offset, setOffset] = useState(0);
  const set = (key: keyof typeof filter) => (e: { target: { value: string } }) => {
    setFilter({ ...filter, [key]: e.target.value });
    setOffset(0);
  };
  const list = useQuery({
    queryKey: ['audit', filter, offset],
    queryFn: () =>
      api<{ entries: AuditEntry[]; total: number }>('/audit', {
        query: { ...filter, from: iso(filter.from), to: iso(filter.to), limit: LIMIT, offset },
      }),
    placeholderData: keepPreviousData,
  });

  return (
    <section>
      <h1>Audit log</h1>
      <div className="card filters">
        <label>
          Actor id
          <input inputMode="numeric" value={filter.actorId} onChange={set('actorId')} />
        </label>
        <label>
          Action
          <input value={filter.action} onChange={set('action')} placeholder="character.rename" />
        </label>
        <label>
          Target type
          <select value={filter.targetType} onChange={set('targetType')}>
            <option value="">any</option>
            <option>account</option>
            <option>character</option>
            <option>realm</option>
          </select>
        </label>
        <label>
          Target id
          <input value={filter.targetId} onChange={set('targetId')} />
        </label>
        <label>
          From
          <input type="datetime-local" value={filter.from} onChange={set('from')} />
        </label>
        <label>
          To
          <input type="datetime-local" value={filter.to} onChange={set('to')} />
        </label>
      </div>
      <ErrorBox error={list.error} />
      {list.data && (
        <>
          <table>
            <thead>
              <tr>
                <th>Time (UTC)</th>
                <th>Actor</th>
                <th>Action</th>
                <th>Target</th>
                <th>Result</th>
                <th>Forced</th>
                <th>Change</th>
              </tr>
            </thead>
            <tbody>
              {list.data.entries.map((e) => (
                <tr key={e.id}>
                  <td>{fmtDate(e.createdAt)}</td>
                  <td>
                    {e.actorEmail} <span className="muted">({e.actorRole})</span>
                  </td>
                  <td>{e.action}</td>
                  <td>
                    {e.targetType} {e.targetId}
                  </td>
                  <td>{e.result === 'ok' ? e.result : <span className="badge danger">{e.result}</span>}</td>
                  <td>{e.forced ? <span className="badge warn">forced</span> : ''}</td>
                  <td>
                    {(e.before != null || e.after != null) && (
                      <details>
                        <summary>show</summary>
                        <pre>{JSON.stringify({ before: e.before, after: e.after }, null, 2)}</pre>
                      </details>
                    )}
                  </td>
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
