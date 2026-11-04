import { useMutation, useQueryClient, type QueryKey } from '@tanstack/react-query';
import { useState, type ReactNode } from 'react';
import { ApiError, type Kicks } from './api';

/** Plain-language names for the API's error codes; anything else shows the code itself. */
const ERROR_LABELS: Record<string, string> = {
  account_banned: 'Account is banned',
  account_locked: 'Account is locked',
  account_not_found: 'Account not found',
  auth_unavailable: 'Auth service unavailable',
  cannot_modify_self: 'You cannot do this to your own account',
  character_not_found: 'Character not found',
  character_online: 'Character is online',
  csrf_invalid: 'Security token expired, reload the page',
  email_taken: 'Email already in use',
  forbidden: 'Not allowed for your role',
  invalid_credentials: 'Wrong email or password',
  name_taken: 'Name already taken on this realm',
  payload_too_large: 'Too large',
  rate_limited: 'Too many attempts, wait a moment',
  realm_name_taken: 'Realm name already in use',
  realm_not_found: 'Realm not found',
  realm_unreachable: 'Realm unreachable',
  unauthorized: 'Signed out',
  validation_error: 'Invalid input',
};

export function ErrorBox({ error }: { error: unknown }) {
  if (!error) return null;
  const err = error instanceof ApiError ? error : new ApiError(0, 'network_error', String(error));
  return (
    <div className="error" role="alert">
      <strong>{ERROR_LABELS[err.code] ?? 'Error'}</strong> <code>{err.code}</code>
      <div>{err.message}</div>
    </div>
  );
}

/** A mutation that refreshes the given queries when it succeeds. */
export function useAction<V, T = unknown>(fn: (v: V) => Promise<T>, invalidate: QueryKey[] = [], onDone?: (t: T) => void) {
  const qc = useQueryClient();
  return useMutation({
    mutationFn: fn,
    onSuccess: async (t) => {
      await Promise.all(invalidate.map((queryKey) => qc.invalidateQueries({ queryKey })));
      onDone?.(t);
    },
  });
}

export function Dialog(props: {
  title: string;
  children?: ReactNode;
  confirmLabel: string;
  danger?: boolean;
  disabled?: boolean;
  error?: unknown;
  onConfirm: () => void;
  onCancel: () => void;
}) {
  return (
    <div className="overlay">
      <div className="dialog card" role="dialog" aria-modal="true" aria-label={props.title}>
        <h2>{props.title}</h2>
        {props.children}
        <ErrorBox error={props.error} />
        <div className="row end">
          <button type="button" className="secondary" onClick={props.onCancel}>
            Cancel
          </button>
          <button type="button" className={props.danger ? 'danger' : ''} disabled={props.disabled} onClick={props.onConfirm}>
            {props.confirmLabel}
          </button>
        </div>
      </div>
    </div>
  );
}

/** A button that opens a confirmation dialog first; with `reason`, the dialog also asks for one. */
export function ConfirmButton<T>(props: {
  label: string;
  title: string;
  danger?: boolean;
  reason?: boolean;
  children?: ReactNode;
  run: (reason: string) => Promise<T>;
  invalidate?: QueryKey[];
  onDone?: (t: T) => void;
}) {
  const [open, setOpen] = useState(false);
  const [reason, setReason] = useState('');
  const m = useAction(props.run, props.invalidate, (t) => {
    setOpen(false);
    props.onDone?.(t);
  });
  return (
    <>
      <button type="button" className={props.danger ? 'danger' : ''} onClick={() => (m.reset(), setOpen(true))}>
        {props.label}
      </button>
      {open && (
        <Dialog
          title={props.title}
          confirmLabel={props.label}
          danger={props.danger}
          disabled={m.isPending || (props.reason === true && !reason.trim())}
          error={m.error}
          onConfirm={() => m.mutate(reason.trim())}
          onCancel={() => setOpen(false)}
        >
          {props.reason && (
            <label>
              Reason
              <input autoFocus value={reason} maxLength={200} onChange={(e) => setReason(e.target.value)} />
            </label>
          )}
          {props.children}
        </Dialog>
      )}
    </>
  );
}

/** Ban, delete and role changes kick online characters; this says how that went. */
export function KickSummary({ kicks }: { kicks: Kicks | null }) {
  if (!kicks) return null;
  return (
    <p className="notice">
      Kicked {kicks.kicked} online session(s).
      {kicks.failures.map((f) => ` Realm ${f.realmId}: ${f.code}.`)}
    </p>
  );
}

export function Pager(props: { total: number; limit: number; offset: number; onChange: (offset: number) => void }) {
  const { total, limit, offset, onChange } = props;
  const last = Math.min(offset + limit, total);
  return (
    <div className="row pager">
      <button type="button" className="secondary" disabled={offset === 0} onClick={() => onChange(Math.max(0, offset - limit))}>
        Previous
      </button>
      <span>{total ? `${offset + 1}-${last} of ${total}` : 'No results'}</span>
      <button type="button" className="secondary" disabled={last >= total} onClick={() => onChange(offset + limit)}>
        Next
      </button>
    </div>
  );
}

export const fmtDate = (iso: string | null | undefined) => (iso ? new Date(iso).toISOString().replace('T', ' ').slice(0, 19) : '-');

export const yesNo = (v: boolean) => (v ? 'yes' : 'no');
