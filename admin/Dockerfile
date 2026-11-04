# --- build: compile the API and the SPA, then drop dev dependencies ---
FROM node:24-bookworm-slim AS build
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci
COPY tsconfig.json tsconfig.build.json ./
COPY vite.config.ts ./
COPY src ./src
COPY web ./web
RUN npm run build && npm prune --omit=dev

# --- runtime: production deps + compiled JS + built SPA, non-root ---
FROM node:24-bookworm-slim
ENV NODE_ENV=production
WORKDIR /app
COPY package.json ./
COPY --from=build /app/node_modules ./node_modules
COPY --from=build /app/dist ./dist
COPY migrations ./migrations
COPY --from=build /app/web/dist ./web/dist
USER node
EXPOSE 3200
HEALTHCHECK --interval=10s --timeout=5s --start-period=20s --retries=3 \
  CMD ["node", "-e", "fetch('http://127.0.0.1:3200/health').then(r => process.exit(r.ok ? 0 : 1), () => process.exit(1))"]
CMD ["node", "dist/server.js"]
