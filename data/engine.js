// Runs inside the hidden YouTube Music page and answers Crimson Music through the crimson message handler
(() => {
	const ORIGIN = 'https://music.youtube.com'
	const LIBRARY_ID = 'FEmusic_liked_playlists'
	const READY_CHECK_MILLISECONDS = 100
	const WATCH_MILLISECONDS = 250
	const GREYED_OUT = 'MUSIC_ITEM_RENDERER_DISPLAY_POLICY_GREY_OUT'
	const PLAYING = 1
	const BUFFERING = 3
	// A song this close to the start counts as starting over
	const START_SECONDS = 1
	const ARTIST_PAGE = 'MUSIC_PAGE_TYPE_ARTIST'
	const ALBUM_PAGE = 'MUSIC_PAGE_TYPE_ALBUM'
	// The browse ids of lyrics start this way
	const LYRICS_PAGE = 'MPLYt'
	// YouTube Music's Android app gets lyrics with the time of each line, which the web client never does
	const CAPTIONS_SOURCE = 'Source: YouTube captions'
	const HOME_ID = 'FEmusic_home'
	const DISCOVER_MIX_TITLE = 'Discover Mix'
	const DISCOVER_MIX_KEY = 'crimson-discover-mix'
	const DISCOVER_TITLE = 'Discover'
	const HISTORY_ID = 'FEmusic_history'
	const HISTORY_TITLE = 'History'
	const HISTORY_DETAIL = 'Songs you played lately'
	const DISCOVER_WEEKS_KEY = 'crimson-discover-weeks'
	const DISCOVER_WEEK_PREFIX = 'discover-week:'
	// Home is read this many pages deep
	const HOME_PAGE_LIMIT = 8
	const COMMENTS_CLIENT = {clientName: 'WEB', clientVersion: '2.20250101.00.00'}
	const AUDIO_TRACK = 'MUSIC_VIDEO_TYPE_ATV'
	const VIDEO_CLASS = 'crimson-video'
	const VIDEO_STYLE_ID = 'crimson-video-style'
	const VIDEO_PATH_CLASS = 'crimson-video-path'
	const VIDEO_SETTLE_MS = 1500
	// The player is lifted out of the page's layout to cover all of it, which also makes YouTube pick a sharper stream,
	// and YouTube's controls, suggestion cards, annotations and end screens are hidden over the video
	const VIDEO_STYLE = `
		html.crimson-video, html.crimson-video body { background: #000 !important; overflow: hidden !important; }
		html.crimson-video body * { visibility: hidden !important; }
		html.crimson-video #movie_player, html.crimson-video #movie_player * { visibility: visible !important; }
		html.crimson-video .crimson-video-path, html.crimson-video #movie_player, html.crimson-video #movie_player video {
			transform: none !important; filter: none !important; contain: none !important; will-change: auto !important;
			opacity: 1 !important; transition: none !important; animation: none !important;
		}
		html.crimson-video .crimson-video-path { z-index: 2147483647 !important; }
		html.crimson-video #movie_player {
			position: fixed !important; inset: 0 !important; width: 100vw !important; height: 100vh !important;
			z-index: 2147483647 !important; background: #000 !important; transform: none !important;
			visibility: visible !important;
		}
		html.crimson-video #movie_player .html5-video-container, html.crimson-video #movie_player video {
			position: absolute !important; left: 0 !important; top: 0 !important; width: 100vw !important;
			height: 100vh !important; margin: 0 !important; object-fit: contain !important;
		}
		html.crimson-video #movie_player .ytp-chrome-top, html.crimson-video #movie_player .ytp-chrome-bottom,
		html.crimson-video #movie_player .ytp-gradient-top, html.crimson-video #movie_player .ytp-gradient-bottom,
		html.crimson-video #movie_player .ytp-pause-overlay, html.crimson-video #movie_player .ytp-ce-element,
		html.crimson-video #movie_player .ytp-cards-teaser, html.crimson-video #movie_player .ytp-cards-button,
		html.crimson-video #movie_player .ytp-suggested-action, html.crimson-video #movie_player .ytp-paid-content-overlay,
		html.crimson-video #movie_player .iv-branding, html.crimson-video #movie_player .annotation,
		html.crimson-video #movie_player .video-annotations, html.crimson-video #movie_player .html5-endscreen,
		html.crimson-video #movie_player .ytp-endscreen-content, html.crimson-video #movie_player .ytp-autonav-endscreen,
		html.crimson-video #movie_player .ytp-overlays-container, html.crimson-video #movie_player .ytp-player-content {
			display: none !important;
		}`
	const TIMED_LYRICS_CLIENT = {clientName: 'ANDROID_MUSIC', clientVersion: '7.27.52', androidSdkVersion: 34}
	const DURATION = /^\d+(:\d\d)+$/
	const EDITABLE_HEADER = 'musicEditablePlaylistDetailHeaderRenderer'
	// The search filters YouTube Music offers as chips above its results
	const SONG_FILTER = 'EgWKAQIIAWoSEAUQCRADEAoQBBAQEA4QFRAR'
	const ARTIST_FILTER = 'EgWKAQIgAWoSEAUQCRADEAoQBBAQEA4QFRAR'
	const ALBUM_FILTER = 'EgWKAQIYAWoSEAUQCRADEAoQBBAQEA4QFRAR'
	const VIDEO_FILTER = 'EgWKAQIQAWoSEAUQCRADEAoQBBAQEA4QFRAR'

	const send = message => window.webkit.messageHandlers.crimson.postMessage(message)
	const getPlayer = () => document.getElementById('movie_player')
	const readText = value => value?.runs?.map(run => run.text).join('') ?? value?.simpleText ?? ''


	function readCookie(name) {
		const prefix = `${name}=`
		const cookie = document.cookie.split('; ').find(entry => entry.startsWith(prefix))
		return cookie?.slice(prefix.length)
	}


	// Depth first search for the first value stored under the key, since YouTube nests renderers differently per page
	function findKey(value, key) {
		if (!value || typeof value !== 'object')
			return undefined
		if (key in value)
			return value[key]
		for (const child of Object.values(value)) {
			const found = findKey(child, key)
			if (found !== undefined)
				return found
		}
		return undefined
	}


	const getArtwork = renderer => findKey(renderer, 'thumbnails')?.at(-1)?.url ?? ''


	async function hashText(text) {
		const digest = await crypto.subtle.digest('SHA-1', new TextEncoder().encode(text))
		return [...new Uint8Array(digest)].map(byte => byte.toString(16).padStart(2, '0')).join('')
	}


	// The page's own request signature, which YouTube asks for on every signed in call
	async function createHeaders() {
		const headers = {
			'Content-Type': 'application/json',
			'X-Origin': ORIGIN,
			'X-Goog-AuthUser': String(ytcfg.get('SESSION_INDEX') ?? 0),
		}
		const visitor = ytcfg.get('VISITOR_DATA')
		if (visitor)
			headers['X-Goog-Visitor-Id'] = visitor
		const secret = readCookie('SAPISID') ?? readCookie('__Secure-3PAPISID')
		if (secret) {
			const now = Math.floor(Date.now() / 1000)
			headers.Authorization = `SAPISIDHASH ${now}_${await hashText(`${now} ${secret} ${ORIGIN}`)}`
		}
		return headers
	}


	// Older lists continue through URL parameters and newer ones through the request body
	async function callApi(endpoint, body, continuation) {
		const query = continuation?.isLegacy
			? `&ctoken=${continuation.token}&continuation=${continuation.token}&type=next`
			: ''
		if (continuation && !continuation.isLegacy)
			body = {continuation: continuation.token}
		const response = await fetch(`/youtubei/v1/${endpoint}?prettyPrint=false${query}`, {
			method: 'POST',
			credentials: 'include',
			headers: await createHeaders(),
			body: JSON.stringify({context: ytcfg.get('INNERTUBE_CONTEXT'), ...body}),
		})
		if (!response.ok)
			throw new Error(`YouTube Music answered ${response.status}`)
		return response.json()
	}


	// Show all lists can run to thousands of uploads, of which an artist page reads this many
	const ALL_SONGS_LIMIT = 1000
	const browse = (body, continuation) => callApi('browse', body, continuation)


	function findContinuation(...places) {
		for (const place of places) {
			const legacy = findKey(place, 'nextContinuationData')?.continuation
			if (legacy)
				return {token: legacy, isLegacy: true}
			const token = findKey(place, 'continuationCommand')?.token
			if (token)
				return {token, isLegacy: false}
		}
		return undefined
	}


	function readPlaylist(item) {
		const renderer = item.musicTwoRowItemRenderer
		const id = renderer?.navigationEndpoint?.browseEndpoint?.browseId
		if (!id?.startsWith('VL'))
			return undefined
		return {
			id: id.slice(2),
			title: readText(renderer.title),
			subtitle: readText(renderer.subtitle),
			artwork: getArtwork(renderer.thumbnailRenderer),
		}
	}


	const getPageType = endpoint =>
		endpoint?.browseEndpoint?.browseEndpointContextSupportedConfigs?.browseEndpointContextMusicConfig?.pageType
	const getRuns = text => text?.runs ?? []


	// Each linked artist as its id and name split by a tab, one to a line, so every name can lead to its own page
	const listArtists = runs => runs.map(run => `${run.navigationEndpoint.browseEndpoint.browseId}\t${run.text}`)
		.join('\n')


	// Songs list their artists and album as linked runs, in a column of their own or joined by dots in search
	function readSong(item) {
		const renderer = item.musicResponsiveListItemRenderer
		if (!renderer || renderer.musicItemRendererDisplayPolicy === GREYED_OUT)
			return undefined
		const id = renderer.playlistItemData?.videoId ?? findKey(renderer.overlay, 'watchEndpoint')?.videoId
		if (!id)
			return undefined
		const columns = renderer.flexColumns?.map(column => column.musicResponsiveListItemFlexColumnRenderer?.text)
		const runs = (columns ?? []).slice(1).flatMap(getRuns)
		// Uploaders link to their channel rather than an artist page, and both open the same way by their UC id
		const artists = runs.filter(run => run.navigationEndpoint?.browseEndpoint?.browseId?.startsWith('UC'))
		const album = runs.find(run => getPageType(run.navigationEndpoint) === ALBUM_PAGE)
		const fixed = renderer.fixedColumns?.[0]?.musicResponsiveListItemFixedColumnRenderer?.text
		const duration = readText(fixed) || runs.map(run => run.text).find(text => DURATION.test(text)) || ''
		return {
			id,
			title: readText(columns?.[0]),
			artist: artists.length ? artists.map(run => run.text).join(', ') : readText(columns?.[1]).split(' • ')[0],
			artistId: listArtists(artists),
			album: album?.text ?? '',
			albumId: album?.navigationEndpoint.browseEndpoint.browseId ?? '',
			duration,
			artwork: getArtwork(renderer.thumbnail),
			setId: renderer.playlistItemData?.playlistSetVideoId ?? '',
			likeStatus: findKey(renderer.menu, 'likeButtonRenderer')?.likeStatus ?? '',
		}
	}


	// Videos sit in carousels as covers, and play as songs like any other, with the channel as their artist
	function readVideo(item, artistId) {
		const renderer = item.musicTwoRowItemRenderer
		const id = renderer?.navigationEndpoint?.watchEndpoint?.videoId
		if (!id)
			return undefined
		const channel = getRuns(renderer.subtitle).find(run => run.navigationEndpoint?.browseEndpoint)
		return {
			id,
			title: readText(renderer.title),
			artist: channel?.text ?? readText(renderer.subtitle).split(' • ')[0],
			artistId: channel?.navigationEndpoint.browseEndpoint.browseId ?? artistId,
			album: '',
			albumId: '',
			duration: '',
			artwork: getArtwork(renderer.thumbnailRenderer),
			setId: '',
		}
	}


	// An artist, album or playlist shown as a cover with a name, from search lists or carousels
	function readCard(item) {
		const renderer = item.musicTwoRowItemRenderer ?? item.musicResponsiveListItemRenderer
		const endpoint = renderer?.navigationEndpoint
		const id = endpoint?.browseEndpoint?.browseId
		if (!id)
			return undefined
		const columns = renderer.flexColumns?.map(column => column.musicResponsiveListItemFlexColumnRenderer?.text)
		return {
			id,
			kind: getPageType(endpoint),
			title: readText(renderer.title ?? columns?.[0]),
			subtitle: readText(renderer.subtitle ?? columns?.[1]),
			artwork: getArtwork(renderer.thumbnailRenderer ?? renderer.thumbnail),
		}
	}


	function findAll(value, key, found = []) {
		if (!value || typeof value !== 'object')
			return found
		if (key in value)
			found.push(value[key])
		Object.values(value).forEach(child => findAll(child, key, found))
		return found
	}


	function readStored(key, fallback) {
		try {
			return JSON.parse(localStorage.getItem(key) ?? 'null') ?? fallback
		} catch {
			return fallback
		}
	}


	function store(key, value) {
		try {
			localStorage.setItem(key, JSON.stringify(value))
		} catch {
			// Without storage nothing is kept between visits
		}
	}


	// YouTube refills the one Discover Mix with new songs each week, so each new set of songs is kept as a week of its
	// own, which Home shows after the current mix, newest first. Its songs are the first page of the mix
	async function keepDiscoverWeek(mix) {
		const response = await browse({browseId: mix.id})
		const songs = (findKey(response, 'musicPlaylistShelfRenderer')?.contents ?? []).map(readSong).filter(Boolean)
		if (!songs.length)
			return
		const weeks = readStored(DISCOVER_WEEKS_KEY, [])
		// The same songs in another order are the same mix
		const signature = songs.map(song => song.id).sort().join(' ')
		if (weeks[0]?.signature === signature || weeks[0]?.signature.split(' ').sort().join(' ') === signature)
			return
		weeks.unshift({date: new Date().toISOString(), signature, artwork: mix.artwork, songs})
		store(DISCOVER_WEEKS_KEY, weeks)
	}


	const describeDay = date => new Date(date).toLocaleDateString(undefined, {day: 'numeric', month: 'short', year: 'numeric'})


	// YouTube can change the mix more than once a day, so mixes from the same day are numbered from the first one
	function describeWeek(week, weeks = readStored(DISCOVER_WEEKS_KEY, [])) {
		const day = describeDay(week.date)
		const sameDay = weeks.filter(item => describeDay(item.date) === day).reverse()
		return sameDay.length > 1 ? `${day} #${sameDay.indexOf(week) + 1}` : day
	}


	// YouTube Music features the Discover Mix on Home only now and then, though the mix itself stays, so it is
	// remembered whenever Home shows it, taken from its shelf and always put first with the weeks kept before. The
	// page's storage goes with the sign in, so another account never sees it
	async function keepDiscoverMix(sections) {
		let mix
		for (const section of sections) {
			const index = section.cards.findIndex(card => card.title === DISCOVER_MIX_TITLE)
			if (index >= 0)
				mix = section.cards.splice(index, 1)[0]
		}
		if (mix)
			store(DISCOVER_MIX_KEY, mix)
		else
			mix = readStored(DISCOVER_MIX_KEY, null)
		for (let index = sections.length - 1; index >= 0; index--) {
			if (!sections[index].cards.length)
				sections.splice(index, 1)
		}
		if (!mix)
			return []
		await keepDiscoverWeek(mix).catch(() => {})
		// The newest week holds the songs the mix has now, so only the ones before it show beside the mix
		const weeks = readStored(DISCOVER_WEEKS_KEY, [])
		const older = weeks.slice(1).map(week => ({
			id: `${DISCOVER_WEEK_PREFIX}${week.date}`,
			kind: '',
			title: DISCOVER_MIX_TITLE,
			subtitle: describeWeek(week, weeks),
			artwork: week.artwork,
		}))
		return [{title: DISCOVER_TITLE, cards: [mix, ...older], round: false}]
	}


	// YouTube Music's listening history, newest first, under headings such as Today that are left out here
	async function loadHistorySongs() {
		const response = await browse({browseId: HISTORY_ID})
		return findAll(response, 'musicShelfRenderer').flatMap(shelf => shelf.contents ?? []).map(readSong)
			.filter(Boolean)
	}


	async function loadHistory(id) {
		const songs = await loadHistorySongs().catch(() => [])
		send({type: 'album', id, title: HISTORY_TITLE, detail: HISTORY_DETAIL, artist: '',
			artwork: songs[0]?.artwork ?? '', songs})
	}


	// A kept week opens like an album, from the songs stored for it
	function loadDiscoverWeek(id) {
		const weeks = readStored(DISCOVER_WEEKS_KEY, [])
		const week = weeks.find(item => `${DISCOVER_WEEK_PREFIX}${item.date}` === id)
		send({type: 'album', id, title: DISCOVER_MIX_TITLE, detail: week ? describeWeek(week, weeks) : '', artist: '',
			artwork: week?.artwork ?? '', songs: week?.songs ?? []})
	}


	// YouTube Music's Home, with the mixes it makes for the listener, quick picks and more, shows a few shelves at first
	// and the rest as it is scrolled, so every page of it is read
	async function loadHome() {
		const sections = []
		const songs = new Map()
		const history = loadHistorySongs().catch(() => [])
		try {
			let page = await browse({browseId: HOME_ID})
			for (let count = 0; page && count < HOME_PAGE_LIMIT; count++) {
				for (const shelf of findAll(page, 'musicCarouselShelfRenderer')) {
					const title = readText(shelf.header?.musicCarouselShelfBasicHeaderRenderer?.title)
					const cards = (shelf.contents ?? []).map(readCard).filter(Boolean)
					if (cards.length)
						sections.push({title, cards, round: cards.every(card => card.kind === ARTIST_PAGE)})
					for (const song of (shelf.contents ?? []).map(readSong).filter(Boolean))
						songs.set(song.id, song)
				}
				const list = findKey(page, 'sectionListRenderer') ?? findKey(page, 'sectionListContinuation')
				const continuation = findContinuation(list?.continuations)
				page = continuation && await browse({}, continuation)
			}
			sections.unshift(...await keepDiscoverMix(sections))
			send({type: 'home', id: HOME_ID, sections, songs: [...songs.values()], history: await history,
				historyId: HISTORY_ID})
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
	}


	// The listener's account picture, which YouTube Music's own account menu shows
	async function loadAccountPhoto() {
		try {
			const menu = await callApi('account/account_menu', {})
			send({type: 'account', photo: findKey(menu, 'accountPhoto')?.thumbnails?.at(-1)?.url ?? ''})
		} catch {
			// Without the picture the Home row keeps its emblem
		}
	}


	async function loadLibrary() {
		try {
			loadAccountPhoto()
			const playlists = []
			let response = await browse({browseId: LIBRARY_ID})
			let grid = findKey(response, 'gridRenderer')
			while (grid) {
				playlists.push(...(grid.items ?? []).map(readPlaylist).filter(Boolean))
				const continuation = findContinuation(grid.continuations, grid.items?.at(-1))
				if (!continuation)
					break
				response = await browse({}, continuation)
				grid = findKey(response, 'gridContinuation') ?? findKey(response, 'appendContinuationItemsAction')
				if (grid?.continuationItems)
					grid = {items: grid.continuationItems}
			}
			send({type: 'playlists', playlists})
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
	}


	// Sends each page as it arrives so the first songs show while the rest of a long playlist loads
	async function loadPlaylist(id) {
		try {
			let response = await browse({browseId: `VL${id}`})
			let shelf = findKey(response, 'musicPlaylistShelfRenderer')
			let items = shelf?.contents ?? []
			let isFirst = true
			const editable = Boolean(findKey(response, EDITABLE_HEADER))
			for (;;) {
				const continuation = findContinuation(shelf?.continuations, items.at(-1))
				const songs = items.map(readSong).filter(Boolean)
				send({type: 'songs', playlist: id, songs, editable, first: isFirst, done: !continuation})
				if (!continuation)
					break
				isFirst = false
				response = await browse({}, continuation)
				shelf = findKey(response, 'musicPlaylistShelfContinuation')
				items = shelf?.contents ?? findKey(response, 'appendContinuationItemsAction')?.continuationItems ?? []
			}
		} catch (error) {
			send({type: 'error', message: String(error), playlist: id})
		}
	}


	// YouTube Music suggests songs for the listener's own playlists in a shelf of its own, which the playlist page
	// only loads once it is scrolled to the end, as the continuation of its sections
	async function loadSuggestions(id) {
		let songs = []
		try {
			const response = await browse({browseId: `VL${id}`})
			const sections = findKey(response?.contents?.twoColumnBrowseResultsRenderer?.secondaryContents,
				'sectionListRenderer')
			const continuation = findContinuation(sections?.continuations)
			if (continuation) {
				const shelf = findKey(await browse({}, continuation), 'musicShelfRenderer')
				songs = (shelf?.contents ?? []).map(readSong).filter(Boolean)
			}
		} catch (error) {
			console.warn('Crimson Music suggestions', error)
		}
		send({type: 'suggestions', playlist: id, songs})
	}


	async function search(query, token) {
		try {
			const filters = [SONG_FILTER, ARTIST_FILTER, ALBUM_FILTER, VIDEO_FILTER]
			const [songs, artists, albums, videos] = await Promise.all(filters.map(params =>
				callApi('search', {query, params}).then(response => findAll(response, 'musicResponsiveListItemRenderer')
					.map(renderer => ({musicResponsiveListItemRenderer: renderer})))))
			send({
				type: 'search',
				token,
				// Videos follow the songs in one list, without any found as a song already
				songs: [...songs, ...videos].map(readSong).filter(Boolean)
					.filter((song, index, all) => all.findIndex(other => other.id === song.id) === index),
				artists: artists.map(readCard).filter(card => card?.kind === ARTIST_PAGE),
				albums: albums.map(readCard).filter(card => card?.kind === ALBUM_PAGE),
			})
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
	}


	// The list behind an artist section's Show all, as a list of songs or a grid of videos, page by page up to a
	// limit, or nothing without one
	async function loadAll(endpoint, artistId) {
		if (!endpoint?.browseId)
			return []
		const songs = []
		const read = item => readSong(item) ?? readVideo(item, artistId)
		let response = await browse({browseId: endpoint.browseId, params: endpoint.params})
		let shelf = findKey(response, 'musicPlaylistShelfRenderer') ?? findKey(response, 'musicShelfRenderer')
			?? findKey(response, 'gridRenderer')
		let items = shelf?.contents ?? shelf?.items ?? []
		while (songs.length < ALL_SONGS_LIMIT) {
			songs.push(...items.map(read).filter(Boolean))
			const continuation = findContinuation(shelf?.continuations, items.at(-1))
			if (!continuation || !items.length)
				break
			response = await browse({}, continuation)
			shelf = findKey(response, 'musicPlaylistShelfContinuation') ?? findKey(response, 'musicShelfContinuation')
				?? findKey(response, 'gridContinuation')
			items = shelf?.contents ?? shelf?.items
				?? findKey(response, 'appendContinuationItemsAction')?.continuationItems ?? []
		}
		return songs
	}


	const findMore = header => findKey(header?.moreContentButton ?? header?.bottomEndpoint ?? header?.title,
		'browseEndpoint')


	// An artist page shows a few of the songs and videos at once, and the full lists behind Show all follow as
	// artist-more, with the songs going after the songs and the videos after the videos
	async function loadArtist(id) {
		try {
			const response = await browse({browseId: id})
			const header = response.header?.musicImmersiveHeaderRenderer ?? response.header?.musicVisualHeaderRenderer
			const shelf = findKey(response, 'musicShelfRenderer')
			const shelves = findAll(response, 'musicCarouselShelfRenderer')
			const carousels = shelves.flatMap(carousel => carousel.contents ?? [])
			const albums = carousels.map(readCard).filter(card => card?.kind === ALBUM_PAGE)
			const known = new Set()
			const fresh = items => items.filter(item => !known.has(item.id) && known.add(item.id))
			const songs = fresh((shelf?.contents ?? []).map(readSong).filter(Boolean))
			const videos = fresh(carousels.map(item => readVideo(item, id)).filter(Boolean))
			send({
				type: 'artist',
				id,
				title: readText(header?.title),
				artwork: getArtwork(header?.thumbnail),
				songs: [...songs, ...videos],
				songCount: songs.length,
				albums,
			})
			const videoShelves = shelves.filter(carousel => carousel.contents?.some(item => readVideo(item, id)))
			const lists = await Promise.all([
				loadAll(findMore({bottomEndpoint: shelf?.bottomEndpoint, title: shelf?.title}), id).catch(() => []),
				...videoShelves.map(carousel => loadAll(findMore(carousel.header?.musicCarouselShelfBasicHeaderRenderer), id)
					.catch(() => [])),
			])
			const moreSongs = fresh(lists[0])
			const moreVideos = fresh(lists.slice(1).flat())
			if (moreSongs.length || moreVideos.length)
				send({type: 'artist-more', id, songs: moreSongs, videos: moreVideos})
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
	}


	// Album tracks carry no covers or album names of their own, so they take the album's
	async function loadAlbum(id) {
		if (id.startsWith(DISCOVER_WEEK_PREFIX))
			return loadDiscoverWeek(id)
		if (id === HISTORY_ID)
			return loadHistory(id)
		try {
			const response = await browse({browseId: id})
			const header = findKey(response, 'musicResponsiveHeaderRenderer') ?? findKey(response, 'musicDetailHeaderRenderer')
			const title = readText(header?.title)
			const artist = readText(header?.straplineTextOne)
			// Album tracks link no artist of their own, so they take every one the album's header links to
			const artistId = listArtists(getRuns(header?.straplineTextOne)
				.filter(run => run.navigationEndpoint?.browseEndpoint?.browseId?.startsWith('UC')))
			const artwork = getArtwork(header?.thumbnail)
			// A playlist, such as a mix opened from Home, lists songs from many albums, which keep their own
			const isPlaylist = id.startsWith('VL')
			const shelf = findKey(response, 'musicShelfRenderer') ?? findKey(response, 'musicPlaylistShelfRenderer')
			const songs = (shelf?.contents ?? []).map(readSong).filter(Boolean)
				.map(song => isPlaylist ? song : {
					...song,
					artist: song.artist || artist,
					artistId: song.artistId || artistId,
					album: title,
					albumId: id,
					artwork: song.artwork || artwork,
				})
			const detail = [artist, readText(header?.subtitle)].filter(Boolean).join(' · ')
			send({type: 'album', id, title, detail, artist, artwork, songs})
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
	}


	const simplify = text => text.toLowerCase().replace(/\s+/g, ' ').trim()


	// Music videos name no album, so a song by the same artist whose title the video's title holds is looked
	// for, the longest such title first, since video titles often add the artist or words like Official Video
	async function findSongAlbum(title, artist) {
		const response = await callApi('search', {query: `${title} ${artist}`, params: SONG_FILTER})
		const songs = findAll(response, 'musicResponsiveListItemRenderer')
			.map(renderer => readSong({musicResponsiveListItemRenderer: renderer})).filter(Boolean)
		const matches = songs.filter(song => song.albumId && simplify(title).includes(simplify(song.title))
			&& simplify(song.artist).includes(simplify(artist)))
		matches.sort((first, second) => second.title.length - first.title.length)
		return matches[0]?.albumId ?? ''
	}


	// Lists often leave a song's album out, while the queue YouTube Music builds for the song names it
	async function findAlbum(video) {
		try {
			const response = await callApi('next', {videoId: video, isAudioOnly: true})
			const entry = findAll(response, 'playlistPanelVideoRenderer').find(item => item.videoId === video)
			const runs = getRuns(entry?.longBylineText)
			const album = runs.find(run => getPageType(run.navigationEndpoint) === ALBUM_PAGE)
			const artist = runs.find(run => getPageType(run.navigationEndpoint) === ARTIST_PAGE)?.text
			let id = album?.navigationEndpoint.browseEndpoint.browseId ?? ''
			if (!id && entry && artist)
				id = await findSongAlbum(readText(entry.title), artist)
			send({type: 'album-found', video, id})
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
	}


	// The timed lines of the lyrics, asked for without the listener's cookies since lyrics are the same for everyone
	async function loadTimedLyrics(browseId) {
		const response = await fetch('/youtubei/v1/browse?prettyPrint=false', {
			method: 'POST',
			credentials: 'omit',
			headers: {'Content-Type': 'application/json'},
			body: JSON.stringify({context: {client: {...TIMED_LYRICS_CLIENT, hl: ytcfg.get('HL') ?? 'en'}}, browseId}),
		})
		const answer = await response.json()
		const data = findKey(answer, 'timedLyricsData') ?? []
		// Some lyrics come back as lines without times, which can only be shown as plain text
		const isTimed = data.length > 0 && data.every(line => line.cueRange?.startTimeMilliseconds !== undefined)
			&& data.some(line => Number(line.cueRange.startTimeMilliseconds) > 0)
		const lines = isTimed ? data.map(line => ({
			text: line.lyricLine ?? '',
			start: Number(line.cueRange.startTimeMilliseconds),
		})) : []
		return {lines, source: findKey(answer, 'sourceMessage') ?? ''}
	}


	// Comments sit under the video on YouTube itself, so they come from YouTube's own web client, which needs no
	// sign in, and in the order YouTube's top comments show
	async function callComments(endpoint, body) {
		const context = ytcfg.get('INNERTUBE_CONTEXT')
		const response = await fetch(`/youtubei/v1/${endpoint}?prettyPrint=false`, {
			method: 'POST',
			credentials: 'include',
			headers: await createHeaders(),
			body: JSON.stringify({context: {...context, client: {...context.client, ...COMMENTS_CLIENT}}, ...body}),
		})
		if (!response.ok)
			throw new Error(`YouTube answered ${response.status}`)
		return response.json()
	}





	function readCommentPage(answer) {
		const items = (answer.onResponseReceivedEndpoints ?? []).flatMap(endpoint =>
			(endpoint.reloadContinuationItemsCommand ?? endpoint.appendContinuationItemsAction)?.continuationItems ?? [])
		const payloads = new Map((answer.frameworkUpdates?.entityBatchUpdate?.mutations ?? [])
			.filter(mutation => mutation.payload?.commentEntityPayload)
			.map(mutation => [mutation.entityKey, mutation.payload.commentEntityPayload]))
		// Top comments come in threads that carry where their replies start, and replies come on their own
		const comments = items.map(item => {
			const thread = item.commentThreadRenderer
			const comment = payloads.get(thread?.commentViewModel?.commentViewModel?.commentKey
				?? item.commentViewModel?.commentKey)
			return comment && {
				author: comment.author?.displayName ?? '',
				text: comment.properties?.content?.content ?? '',
				published: comment.properties?.publishedTime ?? '',
				likes: comment.toolbar?.likeCountNotliked?.trim() ?? '',
				replies: comment.toolbar?.replyCount ? String(comment.toolbar.replyCount) : '',
				repliesToken: findKey(thread?.replies, 'token') ?? '',
			}
		}).filter(Boolean)
		const next = items.at(-1)?.continuationItemRenderer
		return {comments, next: findKey(next, 'token') ?? ''}
	}


	// The first call finds where a video's comments start, and each later one carries on from the last
	async function loadComments(video, token) {
		let page = {comments: [], next: ''}
		try {
			if (!token) {
				const watch = await callComments('next', {videoId: video})
				const section = findAll(watch, 'itemSectionRenderer')
					.find(item => item.sectionIdentifier === 'comment-item-section')
				token = findKey(section, 'token')
			}
			if (token) {
				const answer = await callComments('next', {continuation: token})
				page = readCommentPage(answer)
			}
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
		send({type: 'comments', video, comments: page.comments, next: page.next})
	}


	// A thread's replies come a few at a time, each answer saying where the next ones start
	async function loadReplies(video, token, id) {
		let page = {comments: [], next: ''}
		try {
			page = readCommentPage(await callComments('next', {continuation: token}))
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
		send({type: 'replies', video, id, comments: page.comments, next: page.next})
	}


	// Captions a video carries, which some music videos fill with their words in time, while YouTube's own speech
	// recognition is left out, since it rarely hears lyrics right
	async function loadCaptionLines(video) {
		const response = await callApi('player', {videoId: video})
		const tracks = response?.captions?.playerCaptionsTracklistRenderer?.captionTracks ?? []
		const track = tracks.find(item => item.kind !== 'asr')
		if (!track?.baseUrl)
			return []
		const answer = await (await fetch(`${track.baseUrl}&fmt=json3`)).json()
		return (answer.events ?? []).map(event => ({
			text: (event.segs ?? []).map(segment => segment.utf8 ?? '').join('').replace(/\s+/g, ' ').trim(),
			start: event.tStartMs ?? 0,
		})).filter(line => line.text)
	}


	// A song's words sit behind the Lyrics tab of its watch page, which YouTube marks unselectable when it has none
	async function loadLyrics(video) {
		let text = ''
		let source = ''
		let lines = []
		try {
			const response = await callApi('next', {videoId: video})
			const tab = findAll(response, 'tabRenderer')
				.find(item => item.endpoint?.browseEndpoint?.browseId?.startsWith(LYRICS_PAGE))
			if (tab && !tab.unselectable) {
				const browseId = tab.endpoint.browseEndpoint.browseId
				const lyrics = await callApi('browse', {browseId})
				const shelf = findKey(lyrics, 'musicDescriptionShelfRenderer')
				text = readText(shelf?.description)
				source = readText(shelf?.footer)
				const timed = await loadTimedLyrics(browseId).catch(() => ({lines: [], source: ''}))
				lines = timed.lines
				if (lines.length && timed.source)
					source = typeof timed.source === 'string' ? timed.source : readText(timed.source)
			}
			// Words without times, or none at all, give way to a video's own captions when it has them
			if (!lines.length) {
				const captions = await loadCaptionLines(video).catch(() => [])
				if (captions.length) {
					lines = captions
					source = CAPTIONS_SOURCE
				}
			}
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
		send({type: 'lyrics', video, text, source, lines})
	}


	// A song in the queue YouTube Music builds, as the radio of a song lists them
	function readPanelSong(entry) {
		if (!entry?.videoId)
			return undefined
		const runs = getRuns(entry.longBylineText)
		const artists = runs.filter(run => run.navigationEndpoint?.browseEndpoint?.browseId?.startsWith('UC'))
		const album = runs.find(run => getPageType(run.navigationEndpoint) === ALBUM_PAGE)
		return {
			id: entry.videoId,
			title: readText(entry.title),
			artist: artists.length ? artists.map(run => run.text).join(', ') : readText(entry.shortBylineText),
			artistId: listArtists(artists),
			album: album?.text ?? '',
			albumId: album?.navigationEndpoint.browseEndpoint.browseId ?? '',
			duration: readText(entry.lengthText),
			artwork: getArtwork(entry.thumbnail),
			setId: '',
		}
	}


	// Runs the lookups a few at a time, so a long list neither waits on each in turn nor floods YouTube Music
	async function findEach(items, find, width = 5) {
		const found = []
		for (let start = 0; start < items.length; start += width)
			found.push(...await Promise.all(items.slice(start, start + width).map(item => find(item).catch(() => undefined))))
		return found.filter(Boolean)
	}


	const unique = items => items.filter((item, index) => items.findIndex(other => other.id === item.id) === index)


	// Songs Last.fm finds like this one, each as its best match on YouTube Music, and after them the song's radio,
	// which also covers uploads Last.fm does not know
	async function similarSongs(token, video, list) {
		try {
			const pairs = []
			for (let index = 0; index + 1 < list.length; index += 2)
				pairs.push([list[index], list[index + 1]])
			const named = await findEach(pairs, async ([artist, title]) => {
				const response = await callApi('search', {query: `${title} ${artist}`, params: SONG_FILTER})
				const songs = findAll(response, 'musicResponsiveListItemRenderer')
					.map(renderer => readSong({musicResponsiveListItemRenderer: renderer})).filter(Boolean)
				return songs.find(song => simplify(song.artist).includes(simplify(artist))) ?? songs[0]
			})
			// A song from this computer has no radio, so only Last.fm's matches lead on from it
			const response = video.startsWith('file://') ? {}
				: await callApi('next', {videoId: video, playlistId: `RDAMVM${video}`, isAudioOnly: true})
			const radio = findAll(response, 'playlistPanelVideoRenderer').map(readPanelSong)
				.filter(song => song && song.id !== video)
			send({type: 'similar-songs', token, songs: unique([...named, ...radio].filter(song => song.id !== video))})
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
	}


	// Artists Last.fm finds like this one, each as the artist of that name on YouTube Music, and after them the
	// artists YouTube Music shows on the artist's own page
	async function similarArtists(token, id, names) {
		try {
			const named = await findEach(names, async name => {
				const response = await callApi('search', {query: name, params: ARTIST_FILTER})
				const cards = findAll(response, 'musicResponsiveListItemRenderer')
					.map(renderer => readCard({musicResponsiveListItemRenderer: renderer}))
					.filter(card => card?.kind === ARTIST_PAGE)
				return cards.find(card => simplify(card.title) === simplify(name)) ?? cards[0]
			})
			let related = []
			if (id) {
				const response = await browse({browseId: id})
				related = findAll(response, 'musicCarouselShelfRenderer').flatMap(carousel => carousel.contents ?? [])
					.map(readCard).filter(card => card?.kind === ARTIST_PAGE && card.id !== id)
			}
			send({type: 'similar-artists', token, artists: unique([...named, ...related].filter(card => card.id !== id))})
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
	}


	async function likeSong(video, liked) {
		try {
			await callApi(liked ? 'like/like' : 'like/removelike', {target: {videoId: video}})
			send({type: 'liked', video, liked, ok: true})
		} catch (error) {
			send({type: 'liked', video, liked, ok: false})
		}
	}


	// A dislike takes the place of a like, and taking it away leaves the song neither liked nor disliked
	async function dislikeSong(video, disliked) {
		try {
			await callApi(disliked ? 'like/dislike' : 'like/removelike', {target: {videoId: video}})
			send({type: 'disliked', video, disliked, ok: true})
		} catch (error) {
			send({type: 'disliked', video, disliked, ok: false})
		}
	}


	async function createPlaylist(title) {
		try {
			const response = await callApi('playlist/create', {title, privacyStatus: 'PRIVATE'})
			send({type: 'created', id: response.playlistId ?? ''})
		} catch (error) {
			send({type: 'error', message: String(error)})
		}
	}


	// Adds, removes or moves songs, with actions as YouTube Music's own edit_playlist call takes them
	async function editPlaylist(id, actions) {
		try {
			const response = await callApi('browse/edit_playlist', {playlistId: id, actions})
			// An added song comes back with its entry id, so the app can show it without asking for the playlist again
			const added = response.playlistEditResults?.[0]?.playlistEditVideoAddedResultData
			send({type: 'edited', playlist: id, ok: response.status === 'STATUS_SUCCEEDED', video: added?.videoId,
				setId: added?.setVideoId})
		} catch (error) {
			send({type: 'error', message: String(error), playlist: id})
		}
	}


	let isListening = false


	// The page's video element tells the moment it starts playing or lands after a seek, so starting over
	// waits for no check
	async function waitForPlayer() {
		while (!getPlayer()?.loadVideoById)
			await new Promise(resolve => setTimeout(resolve, READY_CHECK_MILLISECONDS))
		const media = getPlayer().querySelector('video')
		if (media && !isListening) {
			isListening = true
			for (const event of ['playing', 'seeked', 'timeupdate'])
				media.addEventListener(event, watchPlayer)
		}
		return getPlayer()
	}


	// YouTube resumes long songs where they were last stopped, so a song that should start over plays at no
	// volume until it is back at the start, since the player will not start a muted song at all
	let startingOver = ''
	let hasSoughtStart = false
	// The volume Crimson Music asked for, which the page puts back whenever YouTube applies its own remembered one
	let wantedVolume = -1


	// Loading alone leaves the song unstarted when the page was paused, so it is told to play as well
	async function play(id, volume, resume) {
		const player = await waitForPlayer()
		startingOver = resume ? '' : id
		hasSoughtStart = false
		wantedVolume = volume
		player.unMute()
		player.loadVideoById(id)
		player.setVolume(resume ? volume : 0)
		player.playVideo()
	}


	// Only a playing song counts, since while it buffers it still sits at the start before YouTube jumps ahead
	function startOver(player, video, state, time) {
		if (video !== startingOver)
			return
		// Loading a song brings back YouTube's own volume, so it is held at none on every check
		player.setVolume(0)
		if (state !== PLAYING)
			return
		if (time > START_SECONDS) {
			if (!hasSoughtStart)
				player.seekTo(0, true)
			hasSoughtStart = true
			return
		}
		startingOver = ''
		player.setVolume(wantedVolume)
	}


	function keepVolume(player) {
		if (startingOver || wantedVolume < 0 || player.getVolume?.() === wantedVolume)
			return
		player.setVolume(wantedVolume)
	}


	let lastReport = ''


	// Reports the player only when something visible changed, which is every tick while a song plays
	// Songs YouTube Music plays as audio with a still cover are the one kind without a video worth showing
	// A song just loading still has no details for a moment, so the last answer stands until they arrive
	let hasVideo = false
	function isMusicVideo(player, video) {
		const details = player.getPlayerResponse?.()?.videoDetails
		if (details?.videoId === video)
			hasVideo = Boolean(details.musicVideoType) && details.musicVideoType !== AUDIO_TRACK
		return hasVideo
	}


	// The video fills the whole page, with YouTube's own controls hidden, while the app shows the page
	function showVideo(isShown) {
		if (!document.getElementById(VIDEO_STYLE_ID)) {
			const style = document.createElement('style')
			style.id = VIDEO_STYLE_ID
			style.textContent = VIDEO_STYLE
			document.head.append(style)
		}
		// The player sits inside YouTube's hidden, shifted player page, so every layer around it is let out too
		for (let node = getPlayer()?.parentElement; node && node !== document.body; node = node.parentElement)
			node.classList.toggle(VIDEO_PATH_CLASS, Boolean(isShown))
		document.documentElement.classList.toggle(VIDEO_CLASS, Boolean(isShown))
		window.dispatchEvent(new Event('resize'))
		// The app takes its cover away once the page has grown to its new size and the video spans it, with two more
		// frames drawn, or after a longer wait when that never comes
		return new Promise(resolve => {
			const started = performance.now()
			const check = () => {
				const video = getPlayer()?.querySelector('video')?.getBoundingClientRect()
				const spans = innerWidth > 1 && video && Math.abs(video.left) < 1 && Math.abs(video.top) < 1
					&& Math.abs(video.width - innerWidth) < 1 && Math.abs(video.height - innerHeight) < 1
				if (!isShown || performance.now() - started > VIDEO_SETTLE_MS)
					resolve()
				else if (spans)
					requestAnimationFrame(() => requestAnimationFrame(resolve))
				else
					requestAnimationFrame(check)
			}
			requestAnimationFrame(check)
			setTimeout(resolve, VIDEO_SETTLE_MS)
		})
	}


	function watchPlayer() {
		const player = getPlayer()
		if (!player?.getPlayerState)
			return
		const state = player.getPlayerState()
		const video = player.getVideoData?.()?.video_id ?? ''
		const report = {
			type: 'player',
			video,
			state,
			time: player.getCurrentTime?.() ?? 0,
			duration: player.getDuration?.() ?? 0,
			isVideo: isMusicVideo(player, video),
		}
		// A song still on its way back to the start shows as loading at the start, so the bar turns from spinner
		// to pause once, when the song really plays from the beginning
		if (report.video === startingOver) {
			report.time = 0
			report.state = BUFFERING
		}
		startOver(player, report.video, state, player.getCurrentTime?.() ?? 0)
		keepVolume(player)
		const key = JSON.stringify(report)
		if (key === lastReport)
			return
		lastReport = key
		send(report)
	}


	window.crimson = {
		showVideo,
		loadHome,
		loadComments,
		loadReplies,
		loadLibrary,
		loadLyrics,
		loadPlaylist,
		loadSuggestions,
		search,
		similarSongs,
		similarArtists,
		loadArtist,
		loadAlbum,
		findAlbum,
		likeSong,
		dislikeSong,
		createPlaylist,
		addToPlaylist: (id, video) => editPlaylist(id, [{action: 'ACTION_ADD_VIDEO', addedVideoId: video,
			dedupeOption: 'DEDUPE_OPTION_SKIP'}]),
		removeFromPlaylist: (id, video, set) => editPlaylist(id, [{action: 'ACTION_REMOVE_VIDEO', removedVideoId: video,
			setVideoId: set}]),
		renamePlaylist: (id, title) => editPlaylist(id, [{action: 'ACTION_SET_PLAYLIST_NAME', playlistName: title}]),
		deletePlaylist: async id => {
			try {
				await callApi('playlist/delete', {playlistId: id})
				send({type: 'edited', playlist: id, ok: true})
			} catch (error) {
				send({type: 'error', message: String(error), playlist: id})
			}
		},
		moveInPlaylist: (id, set, successor) => editPlaylist(id, [{action: 'ACTION_MOVE_VIDEO_BEFORE', setVideoId: set,
			...(successor ? {movedSetVideoIdSuccessor: successor} : {})}]),
		play,
		pause: () => getPlayer()?.pauseVideo(),
		resume: () => getPlayer()?.playVideo(),
		seek: seconds => getPlayer()?.seekTo(seconds, true),
		setVolume: volume => {
			wantedVolume = volume
			if (!startingOver)
				getPlayer()?.setVolume(volume)
		},
	}


	// The desktop's media keys reach the page through WebKit's own media player, so its controls are handed to the
	// app, which keeps the queue and paces play and pause, including when YouTube sets its handlers again later
	const mediaSession = navigator.mediaSession
	if (mediaSession) {
		const APP_MEDIA_ACTIONS = {nexttrack: 'next', previoustrack: 'previous', play: 'play', pause: 'pause'}
		const setActionHandler = mediaSession.setActionHandler.bind(mediaSession)
		const sendAction = action => () => send({type: 'media-key', action: APP_MEDIA_ACTIONS[action]})
		mediaSession.setActionHandler = (action, handler) =>
			setActionHandler(action, action in APP_MEDIA_ACTIONS ? sendAction(action) : handler)
		for (const action in APP_MEDIA_ACTIONS)
			setActionHandler(action, sendAction(action))
	}


	const readyCheck = setInterval(() => {
		if (!window.ytcfg?.get?.('INNERTUBE_CONTEXT'))
			return
		clearInterval(readyCheck)
		send({type: 'ready', signedIn: Boolean(ytcfg.get('LOGGED_IN'))})
	}, READY_CHECK_MILLISECONDS)
	setInterval(watchPlayer, WATCH_MILLISECONDS)
})()
