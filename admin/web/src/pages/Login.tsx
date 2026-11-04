import { useQueryClient } from '@tanstack/react-query';
import { useState, type FormEvent } from 'react';
import { api, type Caller } from '../api';
import { ErrorBox, useAction } from '../ui';

export function Login() {
  const qc = useQueryClient();
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const login = useAction(
    () => api<Caller>('/session/login', { method: 'POST', body: { email, password } }),
    [],
    (caller) => qc.setQueryData(['session'], caller),
  );
  const submit = (e: FormEvent) => {
    e.preventDefault();
    login.mutate(undefined);
  };

  return (
    <div className="login">
      <form className="card" onSubmit={submit}>
        <h1>Chthonic Admin</h1>
        <p className="muted">Admins and game masters only.</p>
        <label>
          Email
          <input type="email" autoComplete="username" required value={email} onChange={(e) => setEmail(e.target.value)} />
        </label>
        <label>
          Password
          <input
            type="password"
            autoComplete="current-password"
            required
            value={password}
            onChange={(e) => setPassword(e.target.value)}
          />
        </label>
        <ErrorBox error={login.error} />
        <button type="submit" disabled={login.isPending}>
          Log in
        </button>
      </form>
    </div>
  );
}
