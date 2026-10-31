// Removes the ad entries from YouTube's player answers before the page reads them, as Pear Desktop does
(() => {
	const AD_KEYS = ['playerAds', 'adPlacements', 'adSlots']


	function pruneAds(value) {
		if (!value || typeof value !== 'object')
			return value
		for (const owner of [value, value.playerResponse, value.ytInitialPlayerResponse]) {
			if (owner && typeof owner === 'object')
				AD_KEYS.forEach(key => delete owner[key])
		}
		return value
	}


	JSON.parse = new Proxy(JSON.parse, {
		apply: (parse, owner, values) => pruneAds(Reflect.apply(parse, owner, values)),
	})
	Response.prototype.json = new Proxy(Response.prototype.json, {
		apply: (read, owner, values) => Reflect.apply(read, owner, values).then(pruneAds),
	})
	let initialResponse
	Object.defineProperty(window, 'ytInitialPlayerResponse', {
		configurable: true,
		get: () => initialResponse,
		set: value => {
			initialResponse = pruneAds(value)
		},
	})
})()
