import { useQuery, useQueryClient } from '@tanstack/react-query';
import { createContext, useContext } from 'react';
import { Navigate, NavLink, Route, Routes } from 'react-router-dom';
import { api, ApiError, type Caller } from './api';
import { AccountDetail, AccountList, CreateAccount } from './pages/Accounts';
import { Audit } from './pages/Audit';
import { CharacterDetail, CharacterList } from './pages/Characters';
import { Login } from './pages/Login';
import { Instances } from './pages/Instances';
import { Online } from './pages/Online';
import { Realms } from './pages/Realms';
import { ErrorBox } from './ui';

const CallerContext = createContext<Caller | null>(null);

/** The signed-in admin or GM; only used below the login gate. */
export function useCaller(): Caller {
  const caller = useContext(CallerContext);
  if (!caller) throw new Error('useCaller outside a session');
  return caller;
}
export const useIsAdmin = () => useCaller().role === 'admin';

export function App() {
  const qc = useQueryClient();
  // Also hands out the CSRF cookie the login POST needs.
  const session = useQuery({
    queryKey: ['session'],
    queryFn: () =>
      api<Caller>('/session').catch((err: unknown) => {
        if (err instanceof ApiError && err.status === 401) return null;
        throw err;
      }),
  });

  if (session.isPending) return null;
  if (session.isError) return <ErrorBox error={session.error} />;
  const caller = session.data;
  if (!caller) return <Login />;

  const logout = async () => {
    await api('/session/logout', { method: 'POST' }).catch(() => {});
    qc.clear();
    qc.setQueryData(['session'], null);
  };
  const admin = caller.role === 'admin';

  return (
    <CallerContext.Provider value={caller}>
      <header className="topbar">
        <span className="brand">Chthonic Admin</span>
        <nav>
          <NavLink to="/accounts">Accounts</NavLink>
          <NavLink to="/characters">Characters</NavLink>
          <NavLink to="/online">Online</NavLink>
          <NavLink to="/instances">Instances</NavLink>
          {admin && <NavLink to="/realms">Realms</NavLink>}
          {admin && <NavLink to="/audit">Audit</NavLink>}
        </nav>
        <span className="who">
          {caller.email} <span className="badge">{caller.role}</span>
          <button type="button" className="secondary" onClick={logout}>
            Log out
          </button>
        </span>
      </header>
      <main>
        <Routes>
          <Route path="/accounts" element={<AccountList />} />
          <Route path="/accounts/new" element={admin ? <CreateAccount /> : <Navigate to="/accounts" />} />
          <Route path="/accounts/:id" element={<AccountDetail />} />
          <Route path="/characters" element={<CharacterList />} />
          <Route path="/characters/:id" element={<CharacterDetail />} />
          <Route path="/online" element={<Online />} />
          <Route path="/instances" element={<Instances />} />
          {admin && <Route path="/realms" element={<Realms />} />}
          {admin && <Route path="/audit" element={<Audit />} />}
          <Route path="*" element={<Navigate to="/accounts" replace />} />
        </Routes>
      </main>
    </CallerContext.Provider>
  );
}
