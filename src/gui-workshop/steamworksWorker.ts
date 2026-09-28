// steamworksWorker.ts — utilityProcess worker isolating steamworks.
//
// Runs inside an Electron utilityProcess: Valve's breakpad crash hooks live
// only here, isolated from Chromium's process/fd management. Whatever happens
// to this process, the UI process stays unaffected; the parent restarts this
// worker on demand and degrades gracefully.
//
// Protocol:
//   parent -> worker: { type: "init", appId }
//                     { type: "call", id, method, params }
//   worker -> parent:  { type: "ready" } | { type: "initError", error }
//                      { type: "result", id, result } | { type: "result", id, error }

import { init } from "steamworks.js";

let client: any = null;
const parentPort: any = (process as any).parentPort;

const methods: Record<string, (params: any) => any> = {
	async getItems({ ids, opts }: any) {
		return client.workshop.getItems(
			ids.map((id: string) => BigInt(id)),
			opts,
		);
	},

	// Workshop search: upstream retries multiple queryType/itemType
	// combinations and merges pages; the whole loop lives here so the parent
	// needs a single RPC call.
	async searchAllItems(p: any) {
		const { startingSteamPage, queryTypesToTry, itemTypesToTry, appId, queryConfig, steamPagesPerFrontendPage, requestedPageSize, requestedQueryType, requestedItemType } = p;

		let totalResults = 0;
		const allMergedItems: any[] = [];
		let bestQT = requestedQueryType;
		let bestIT = requestedItemType;
		let typesFound = false;

		for (const qt of queryTypesToTry) {
			for (const it of itemTypesToTry) {
				try {
					const res = await client.workshop.getAllItems(
						startingSteamPage, qt, it, appId, appId, queryConfig,
					);
					if ((res?.items || []).length > 0 || (res?.totalResults || 0) > 0) {
						bestQT = qt;
						bestIT = it;
						typesFound = true;
						totalResults = res?.totalResults || 0;
						allMergedItems.push(...(res?.items || []).filter((i: any) => i));
						break;
					}
				} catch (e: any) {
					console.error(`[sw-worker] getAllItems failed (queryType=${qt}, itemType=${it}):`, e?.message ?? e);
				}
			}
			if (typesFound) break;
		}

		if (typesFound && steamPagesPerFrontendPage > 1) {
			for (let i = 1; i < steamPagesPerFrontendPage; i++) {
				const currentSteamPage = startingSteamPage + i;
				if (allMergedItems.length >= totalResults && totalResults > 0) break;
				try {
					const res = await client.workshop.getAllItems(
						currentSteamPage, bestQT, bestIT, appId, appId, queryConfig,
					);
					if (res?.items) {
						allMergedItems.push(...res.items.filter((i: any) => i));
					}
				} catch (e: any) {
					console.error(`[sw-worker] page fetch failed (steam page ${currentSteamPage}):`, e?.message ?? e);
				}
			}
		}

		if (allMergedItems.length > requestedPageSize) {
			allMergedItems.length = requestedPageSize;
		}
		return { items: allMergedItems, totalResults };
	},

	async getItem({ id, opts }: any) {
		return client.workshop.getItem(BigInt(id), opts);
	},

	async subscribe({ id }: any) {
		await client.workshop.subscribe(BigInt(id));
		return { success: true };
	},

	async unsubscribe({ id }: any) {
		await client.workshop.unsubscribe(BigInt(id));
		return { success: true };
	},

	getSubscribedItems() {
		return client.workshop.getSubscribedItems();
	},

	getDownloadingItems() {
		const subscribedItems = client.workshop.getSubscribedItems();
		const downloadingItems: any[] = [];
		for (const fileId of subscribedItems) {
			const state = client.workshop.state(fileId);
			// 16 = Downloading, 32 = Download Pending
			if ((state & 16) || (state & 32)) {
				const info = client.workshop.downloadInfo(fileId);
				if (info) {
					downloadingItems.push({
						fileId: fileId.toString(),
						current: info.current.toString(),
						total: info.total.toString(),
						state,
					});
				}
			}
		}
		return downloadingItems;
	},

	getDownloadInfo({ id }: any) {
		const info = client.workshop.downloadInfo(BigInt(id));
		return info ? { current: String(info.current), total: String(info.total) } : null;
	},

	getInstallInfo({ id }: any) {
		const info = client.workshop.installInfo(BigInt(id));
		return info ? { ...info, sizeOnDisk: String(info.sizeOnDisk) } : null;
	},
};

parentPort.on("message", async (event: any) => {
	const msg = event?.data ?? event;

	if (msg?.type === "init") {
		try {
			client = init(msg.appId);
			parentPort.postMessage({ type: "ready" });
		} catch (err: any) {
			client = null;
			parentPort.postMessage({ type: "initError", error: String(err?.message ?? err) });
		}
		return;
	}

	if (msg?.type === "call") {
		const handler = methods[msg.method];
		if (!handler) {
			parentPort.postMessage({ type: "result", id: msg.id, error: `unknown method: ${msg.method}` });
			return;
		}
		if (!client) {
			parentPort.postMessage({ type: "result", id: msg.id, error: "steamworks not initialized" });
			return;
		}
		try {
			const result = await handler(msg.params ?? {});
			parentPort.postMessage({ type: "result", id: msg.id, result });
		} catch (err: any) {
			parentPort.postMessage({ type: "result", id: msg.id, error: String(err?.message ?? err) });
		}
	}
});
