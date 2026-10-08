import { ipcMain, utilityProcess, type UtilityProcess } from "electron";
import { readdirSync, readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { logger } from "../logger";
import { WALLPAPER_ENGINE_APP_ID } from "../../shared/constants";
import { socketClient } from "../socket-client";

export interface WorkshopQueryOptions {
	query_type?: number;
	page?: number;
	cursor?: string;
	numperpage?: number;
	requiredtags?: string[];
	excludedtags?: string[];
	match_all_tags?: boolean;
	search_text?: string;
	return_details?: boolean;
	return_tags?: boolean;
	return_previews?: boolean;
	item_type?: number;
}

// Lightweight probe: is the Steam *client process* running?
// Deliberately does NOT touch steamworks initialization.
function isSteamClientProcessRunning(): boolean {
	try {
		for (const pid of readdirSync("/proc")) {
			if (!/^\d+$/.test(pid)) continue;
			try {
				const comm = readFileSync(`/proc/${pid}/comm`, "utf8").trim();
				if (comm === "steam" || comm === "steamwebhelper") return true;
			} catch {
				// process exited or not readable; skip
			}
		}
	} catch {
		// /proc unavailable
	}
	return false;
}

const SERVICE_DIR = dirname(fileURLToPath(import.meta.url));
const WORKER_PATH = join(SERVICE_DIR, "steamworksWorker.js");
const WORKER_INIT_TIMEOUT = 15000;
const WORKER_CALL_TIMEOUT = 30000;
const WORKER_RETRY_COOLDOWN = 30000;
const WORKER_SAFE_WINDOW = 2000;
// WorkshopWorker — RPC client that isolates steamworks inside a
// utilityProcess worker.
//
// Rationale: Valve's steam_api installs breakpad crash hooks into the host
// process; their watcher pipe gets cut by Chromium's process/fd management
// seconds after startup, and steam_api then fatally exits the host. The SDK
// must therefore never initialize inside the UI process — it lives only in
// the steamworksWorker.js utility process; if that worker dies, Workshop
// degrades gracefully while the UI process stays unaffected.
//
// The Flatpak Steam client only guarantees a ~2s IPC-safe window after
// worker init; UGC calls outside that window trip an internal steam_api
// assert that kills the worker process. The worker therefore retires
// itself on a TTL and retries each failed call once with a fresh worker.
class WorkshopWorker {
	private child: any = null;
	private nextId = 0;
	private pending = new Map<number, { resolve: (value: any) => void; reject: (error: any) => void; timer: NodeJS.Timeout }>();
	private unavailableUntil = 0;
	private bornAt = 0;
	private starting: Promise<UtilityProcess | null> | null = null;
	// Availability probe for passive pollers: true only while a worker is
	// already up and not cooling down — must never spawn one.
	isAvailable(): boolean {
		return this.child !== null && Date.now() >= this.unavailableUntil;
	}

	async call<T = any>(method: string, params?: any, allowRetry = true): Promise<T> {
		try {
			const child = await this.ensure();
			if (!child) {
				throw new Error("steamworks unavailable");
			}
			return await this.rawCall<T>(method, params);
		} catch (msg: any) {
			// If the worker gets killed mid-call by the steam_api assert, the call is
			// retried once with a fresh worker; other errors (e.g. Steam not running)
			// are terminal and propagate.
			if (!allowRetry || !/worker exited/.test(String(msg))) throw msg;
			this.child = null;
			this.unavailableUntil = Date.now() + WORKER_RETRY_COOLDOWN;
			await new Promise((r) => setTimeout(r, WORKER_RETRY_COOLDOWN));
			return await this.call(method, params, false);
		}
	}

	private async rawCall<T = any>(method: string, params?: any): Promise<T> {
		const child = await this.ensure();
		if (!child) {
			throw new Error("steamworks unavailable");
		}

		const id = this.nextId++;
		return new Promise<T>((resolve, reject) => {
			const timer = setTimeout(() => {
				this.pending.delete(id);
				reject(new Error(`steamworks rpc timeout: ${method}`));
			}, WORKER_CALL_TIMEOUT);
			this.pending.set(id, { resolve, reject, timer });
			child.postMessage({ type: "call", id, method, params });
		});
	}

	private ensure(): Promise<UtilityProcess | null> {
		if (this.child) {
			if (Date.now() - this.bornAt <= WORKER_SAFE_WINDOW) {
				return Promise.resolve(this.child);
			}
			// TTL exceeded: retire before the steam_api assert fires
			try {
				this.child.kill();
			} catch {
				// already gone
			}
			this.child = null;
		}
		if (Date.now() < this.unavailableUntil) return Promise.resolve(null);
		if (this.starting) return this.starting.then(() => this.child);
		this.starting = this.start().then(() => this.child);
		return this.starting;
	}

	private start(): Promise<boolean> {
		return new Promise<boolean>((resolve) => {
			let child: UtilityProcess;
			try {
				child = utilityProcess.fork(WORKER_PATH, [], {
					serviceName: "steamworks-worker",
					stdio: "inherit",
				});
			} catch (err: any) {
				logger.error(`Failed to fork steamworks worker: ${err?.message ?? err}`);
				this.unavailableUntil = Date.now() + WORKER_RETRY_COOLDOWN;
				return resolve(false);
			}

			this.child = child;
			this.bornAt = Date.now();
			logger.backend("Starting steamworks utility process...");

			let settled = false;
			const finish = (ok: boolean) => {
				if (settled) return;
				settled = true;
				clearTimeout(guard);
				if (!ok) {
					this.unavailableUntil = Date.now() + WORKER_RETRY_COOLDOWN;
				}
				resolve(ok);
			};

			// init handshake timeout: kill the worker and enter cooldown
			const guard = setTimeout(() => {
				try {
					child.kill();
				} catch {
					// already gone
				}
				finish(false);
			}, WORKER_INIT_TIMEOUT);

			child.on("message", (msg: any) => {
				if (settled && msg?.type !== "result") return;
				if (msg?.type === "ready") {
					finish(true);
					return;
				}
				if (msg?.type === "initError") {
					logger.backend(`Steamworks worker init failed: ${msg.error}`);
					try {
						child.kill();
					} catch {
						// already gone
					}
					finish(false);
					return;
				}
				if (msg?.type === "result") {
					const entry = this.pending.get(msg.id);
					if (!entry) return;
					clearTimeout(entry.timer);
					this.pending.delete(msg.id);
					if (msg.error) entry.reject(new Error(msg.error));
					else entry.resolve(msg.result);
				}
			});

			child.on("exit", () => {
				if (this.child === child) this.child = null;
				for (const [, entry] of this.pending) {
					clearTimeout(entry.timer);
					entry.reject(new Error("steamworks worker exited"));
				}
				this.pending.clear();
				finish(false);
			});

			child.postMessage({ type: "init", appId: WALLPAPER_ENGINE_APP_ID });
		});
	}
}

export function registerWorkshopService() {
	const worker = new WorkshopWorker();

	ipcMain.handle("is-steam-running", async () => {
		// answers whether the Steam client process runs; never initializes steamworks
		return isSteamClientProcessRunning();
	});

	ipcMain.handle(
		"get-published-file-details",
		async (_, fileIds: string[]) => {
			logger.ipcReceived("get-published-file-details", fileIds);

			if (!fileIds || fileIds.length === 0) {
				return [];
			}

			try {
				const result = await worker.call<any>("getItems", {
					ids: fileIds,
					opts: {
						includeMetadata: true,
						includeAdditionalPreviews: true
					}
				});

				return (result?.items || []).filter((it: any) => it).map((it: any) => ({
					...it,
					publishedfileid: it.publishedFileId?.toString(),
					result: 1,
					image: it.previewUrl,
					preview_url: it.previewUrl,
					time_created: it.timeCreated,
					time_updated: it.timeUpdated,
					subscriptions: Number(it.statistics?.numSubscriptions || 0),
					favorites: Number(it.statistics?.numFavorites || 0),
					views: Number(it.statistics?.numUniqueWebsiteViews || 0),
				}));
			} catch (error: any) {
				logger.error("Error fetching published file details:", error?.message ?? error);
				throw error;
			}
		},
	);

	ipcMain.handle(
		"query-workshop-files",
		async (_: any, options: WorkshopQueryOptions = {}) => {
			logger.ipcReceived("query-workshop-files", options);

			try {
				const requestedPageSize = options.numperpage ?? 50;
				const frontendPage = options.page ?? 1;
				const requestedQueryType = options.query_type ?? 13;
				const requestedItemType = options.item_type ?? 13;

				const steamPagesPerFrontendPage = Math.ceil(requestedPageSize / 50);
				const startingSteamPage = (frontendPage - 1) * steamPagesPerFrontendPage + 1;

				const queryConfig: any = {
					requiredTags: options.requiredtags || undefined,
					excludedTags: options.excludedtags || undefined,
					matchAnyTag: options.match_all_tags === true ? false : true,
					searchText: options.search_text || undefined,
					includeMetadata: true,
					includeAdditionalPreviews: true,
					includeLongDescription: false,
					numPerPage: 50,
					cachedResponseMaxAge: 0,
				};

				if (Array.isArray(queryConfig.requiredTags) && queryConfig.requiredTags.length > 3) {
					queryConfig.matchAnyTag = true;
				}

				const res = await worker.call<any>("searchAllItems", {
					startingSteamPage,
					queryTypesToTry: Array.from(new Set([requestedQueryType, 13, 1, 2, 9])),
					itemTypesToTry: [requestedItemType, 13, 0],
					appId: WALLPAPER_ENGINE_APP_ID,
					queryConfig,
					steamPagesPerFrontendPage,
					requestedPageSize,
					requestedQueryType,
					requestedItemType,
				});

				const mappedItems = (res?.items || []).map((it: any) => ({
					...it,
					publishedfileid: it.publishedFileId?.toString(),
					result: 1,
					image: it.previewUrl,
					preview_url: it.previewUrl,
					time_created: it.timeCreated,
					time_updated: it.timeUpdated,
					subscriptions: Number(it.statistics?.numSubscriptions || 0),
					favorites: Number(it.statistics?.numFavorites || 0),
					views: Number(it.statistics?.numUniqueWebsiteViews || 0),
				}));

				return {
					items: mappedItems,
					total: res?.totalResults || 0,
					nextCursor: null,
				};
			} catch (error: any) {
				logger.error("Error in query-workshop-files:", error?.message ?? error);
				return { items: [], total: 0, nextCursor: null, error: error?.message };
			}
		},
	);

	ipcMain.handle(
		"get-ugc-file-details",
		async (_, ugcId: string) => {
			logger.ipcReceived("get-ugc-file-details", ugcId);

			if (!ugcId) {
				throw new Error("UGC ID is required");
			}

			try {
				const it = await worker.call<any>("getItem", {
					id: ugcId,
					opts: {
						includeMetadata: true,
						includeAdditionalPreviews: true,
						includeLongDescription: true
					}
				});

				if (!it) return null;

				return {
					...it,
					publishedfileid: it.publishedFileId?.toString(),
					result: 1,
					image: it.previewUrl,
					preview_url: it.previewUrl,
					time_created: it.timeCreated,
					time_updated: it.timeUpdated,
					subscriptions: Number(it.statistics?.numSubscriptions || 0),
					favorites: Number(it.statistics?.numFavorites || 0),
					views: Number(it.statistics?.numUniqueWebsiteViews || 0),
					fileSize: Number(it.fileSize || it.file_size || 0),
				};
			} catch (error: any) {
				logger.error("Error fetching UGC file details:", error?.message ?? error);
				throw error;
			}
		},
	);

	ipcMain.handle("subscribe-workshop-item", async (_, fileId: string) => {
		logger.ipcReceived("subscribe-workshop-item", fileId);

		try {
			await worker.call("subscribe", { id: fileId });
			return { success: true };
		} catch (error: any) {
			logger.error(`Error subscribing to item ${fileId}:`, error?.message ?? error);
			throw error;
		}
	});

	ipcMain.handle("unsubscribe-workshop-item", async (_, fileId: string) => {
		logger.ipcReceived("unsubscribe-workshop-item", fileId);

		try {
			// Kill the wallpaper before unsubscribing
			try {
				await socketClient.send("kill-wallpaper", { folderName: fileId });
			} catch (e: any) {
				logger.error(`Failed to kill wallpaper ${fileId} before unsubscription:`, e);
			}

			await worker.call("unsubscribe", { id: fileId });
			return { success: true };
		} catch (error: any) {
			logger.error(`Error unsubscribing from item ${fileId}:`, error?.message ?? error);
			throw error;
		}
	});

	ipcMain.handle("get-all-downloading-items", async () => {
		// passive polling: return empty while the worker is down, never spawn it
		if (!worker.isAvailable()) return [];
		try {
			return await worker.call("getDownloadingItems");
		} catch (error: any) {
			logger.error("Error fetching all downloading items:", error?.message ?? error);
			return [];
		}
	});

	ipcMain.handle("get-subscribed-items", async () => {
		// worker available -> real subscription list; otherwise fall back to
		// locally downloaded IDs so the home list stays complete
		if (worker.isAvailable()) {
			try {
				const subscribedItems = await worker.call<string[]>("getSubscribedItems");
				return subscribedItems.map((id) => id.toString());
			} catch (error: any) {
				logger.error("Error fetching subscribed items:", error?.message ?? error);
			}
		}

		try {
			const basePath = await socketClient.send("get-wallpaper-base-path");
			if (typeof basePath !== "string" || !basePath) return [];
			return readdirSync(basePath).filter((name) => /^\d+$/.test(name));
		} catch {
			return [];
		}
	});

	ipcMain.handle(
		"get-workshop-item-download-info",
		async (_, fileId: string) => {
			if (!worker.isAvailable()) return null;
			try {
				return await worker.call("getDownloadInfo", { id: fileId });
			} catch (error: any) {
				return null;
			}
		},
	);

	ipcMain.handle("get-workshop-item-install-info", async (_, fileId: string) => {
		if (!worker.isAvailable()) return null;
		try {
			return await worker.call("getInstallInfo", { id: fileId });
		} catch (error: any) {
			return null;
		}
	});
}
