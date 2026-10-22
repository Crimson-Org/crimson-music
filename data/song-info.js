// Gathers what is known about one song from YouTube, Deezer, MusicBrainz, ReccoBeats and Last.fm, one request at a
// time.
// The app runs every request it is asked for, paced per service, and hands each answer back, so all choices of
// what to ask next and how to read the answers live here.

'use strict'

// Raising this fetches every stored song again, for when the reading below learns something new
const VERSION = 8

const YOUTUBE = 'https://www.youtube.com/youtubei/v1/next?prettyPrint=false'
// Answers even without a sign in with the uploader's keywords, though it refuses to play the video
const YOUTUBE_PLAYER = 'https://www.youtube.com/youtubei/v1/player?prettyPrint=false'
const YOUTUBE_CLIENT = {clientName: 'WEB', clientVersion: '2.20251001.00.00', hl: 'en', gl: 'US'}
const DEEZER = 'https://api.deezer.com/'
const MUSICBRAINZ = 'https://musicbrainz.org/ws/2/'
// Every genre MusicBrainz knows, one per line, which picks the genres out of an upload's keywords and title
const GENRE_LIST = MUSICBRAINZ + 'genre/all?fmt=txt'
// Words uploaders put after a genre, as in "synthwave music" or "lofi beats"
const GENRE_SUFFIX = /\s+(music|mix|beats?|songs?|playlist|radio|type beat)$/
// Audio analysis of a song by its ISRC, free and without a key, with the tempo, key and how the song feels
const RECCOBEATS = 'https://api.reccobeats.com/v1/audio-features?ids='
// Tags listeners gave the song, or else its artist, where the app's own key is set as LASTFM_KEY before this runs
const LASTFM = 'https://ws.audioscrobbler.com/2.0/?format=json&autocorrect=1&api_key='
// A tag counts from this weight on, where Last.fm weighs a song's tags from 0 to 100
const LASTFM_WEIGHT = 10
// Tags that say nothing about the music itself, which listeners often add
const IGNORED_TAGS = new Set(['seen live', 'favorites', 'favourites', 'favorite', 'favourite', 'my favorites', 'love',
	'awesome', 'beautiful', 'cool', 'good', 'amazing', 'best', 'albums i own', 'spotify'])
const PITCHES = ['C', 'C♯', 'D', 'D♯', 'E', 'F', 'F♯', 'G', 'G♯', 'A', 'A♯', 'B']
// Each mood in percent, from the analysis's levels between 0 and 1, where calm and sad are the low ends of energy
// and positivity
const MOODS = {
	danceable: features => features.danceability,
	energetic: features => features.energy,
	calm: features => 1 - features.energy,
	happy: features => features.valence,
	sad: features => 1 - features.valence,
	acoustic: features => features.acousticness,
	instrumental: features => features.instrumentalness,
}
// A match on Deezer may differ this much in length, since uploads often add a second or two of silence
const LENGTH_TOLERANCE = 6
// MusicBrainz scores its search results out of 100, and anything lower than this is a different song or artist
const MUSICBRAINZ_SCORE = 95
const TAG_LIMIT = 12

// Known albums and artists are not asked for again while the app runs
const deezerAlbums = new Map()
const lastfmArtists = new Map()
const artistTags = new Map()
// Each genre's name by its simplified spelling, so "lofi", "Lo-Fi" and "lo fi" all find "lo-fi"
let genres = null


function findAll(value, key, found = []) {
	if (Array.isArray(value)) {
		for (const item of value)
			findAll(item, key, found)
	} else if (value && typeof value === 'object') {
		for (const [name, child] of Object.entries(value)) {
			if (name === key)
				found.push(child)
			findAll(child, key, found)
		}
	}
	return found
}


function parse(text) {
	try {
		return text ? JSON.parse(text) : null
	} catch {
		return null
	}
}


// A title without brackets and featured artists, which is how the services name a song
function plainTitle(title) {
	return (title || '').replace(/\([^)]*\)|\[[^\]]*\]/g, ' ').replace(/\s(feat|ft|featuring)\b.*$/i, ' ')
		.replace(/\s+/g, ' ').trim()
}


// Lowercase letters and digits only, with brackets and featured artists dropped, so two spellings of one title
// compare equal
function simplify(text) {
	return (text || '').toLowerCase()
		.replace(/\([^)]*\)|\[[^\]]*\]/g, ' ')
		.replace(/\b(feat|ft|featuring)\b.*$/, ' ')
		.normalize('NFKD').replace(/[^\p{L}\p{N}]+/gu, '')
}


function isSameText(one, other) {
	const a = simplify(one), b = simplify(other)
	return a.length > 0 && b.length > 0 && (a === b || (Math.min(a.length, b.length) >= 4 && (a.includes(b) || b.includes(a))))
}


// The first of several credited artists, which is the one the services file the song under, while a name such as
// "Ashes & Echoes" stays whole
function firstArtist(artist) {
	return (artist || '').split(/,| feat\.? | ft\.? /i)[0].trim()
}


function readSeconds(duration) {
	if (!duration)
		return 0
	return duration.split(':').reduce((total, part) => total * 60 + Number(part), 0) || 0
}


function toDate(text) {
	const time = Date.parse(text)
	return Number.isNaN(time) ? '' : new Date(time).toISOString().slice(0, 10)
}


// Lines such as "0:00 Intro" or "Intro - 1:02:03" in a description, used when YouTube has no chapters of its own,
// and only when the list starts at the beginning
function readDescriptionChapters(description) {
	const chapters = []
	for (const line of (description || '').split('\n')) {
		const match = line.match(/(?:^|\s|\()((?:\d{1,2}:)?\d{1,2}:\d{2})(?:\)|\s|$)/)
		if (!match)
			continue
		const title = line.replace(match[0], ' ').replace(/^[\s\d.\-–—|:)]+|[\s\-–—|:(]+$/g, '').trim()
		chapters.push({title: title || match[1], start: readSeconds(match[1])})
	}
	const isOrdered = chapters.every((chapter, index) => index === 0 || chapter.start > chapters[index - 1].start)
	return chapters.length >= 2 && chapters[0].start === 0 && isOrdered ? chapters : []
}


function readYouTube(job, answer) {
	if (!answer)
		return
	job.sources.add('YouTube')
	const chapters = new Map()
	for (const chapter of findAll(answer, 'chapterRenderer')) {
		const start = Math.round((chapter.timeRangeStartMillis || 0) / 1000)
		if (!chapters.has(start))
			chapters.set(start, {title: chapter.title?.simpleText || '', start})
	}
	const description = findAll(answer, 'attributedDescription')[0]?.content || ''
	const found = [...chapters.values()].sort((a, b) => a.start - b.start)
	job.info.chapters = found.length >= 2 ? found : readDescriptionChapters(description)
	job.description = description
	const date = findAll(answer, 'dateText')[0]?.simpleText || ''
	job.info.uploaded = toDate(date.replace(/^[^\d]*?(?=[A-Z][a-z]{2} \d)/, ''))
	const views = findAll(answer, 'videoViewCountRenderer')[0]?.viewCount?.simpleText || ''
	job.info.views = Number(views.replace(/[^\d]/g, '')) || 0
	job.info.channel = findAll(answer, 'videoOwnerRenderer')[0]?.title?.runs?.[0]?.text || ''
}


function isSameLength(job, seconds) {
	return !job.seconds || Math.abs(seconds - job.seconds) <= LENGTH_TOLERANCE
}


// Picks the Deezer result with the same title, artist and length, preferring the same album and then the most
// played one, which is the original release rather than a later reissue
function pickDeezer(job, answer) {
	const matches = (answer?.data || []).filter(track => isSameText(track.title, job.title)
		&& isSameText(track.artist?.name, job.artistName) && isSameLength(job, track.duration))
	const album = simplify(job.info.album)
	const score = track => (simplify(track.album?.title) === album ? 2e12 : 0)
		+ (isSameText(track.album?.title, job.info.album) ? 1e12 : 0) + (track.rank || 0)
	return matches.sort((a, b) => score(b) - score(a))[0] || null
}


function earliest(...dates) {
	return dates.filter(Boolean).sort()[0] || ''
}


function readDeezerTrack(job, answer) {
	if (!answer || answer.error)
		return
	job.sources.add('Deezer')
	job.info.bpm = answer.bpm > 0 ? Math.round(answer.bpm) : 0
	job.info.gain = typeof answer.gain === 'number' ? answer.gain : null
	job.info.isrc = answer.isrc || ''
	job.info.released = earliest(job.info.released, answer.release_date)
	job.info.explicit = answer.explicit_lyrics ? 1 : 0
	job.info.album = job.info.album || answer.album?.title || ''
}


function readDeezerAlbum(job, album) {
	if (!album)
		return
	job.info.genres = (album.genres?.data || []).map(genre => genre.name)
	job.info.label = album.label || ''
	job.info.cover = album.cover_xl || ''
	job.info.released = earliest(job.info.released, album.release_date)
}


function readTags(entry) {
	return (entry?.tags || []).filter(tag => tag.count > 0).sort((a, b) => b.count - a.count)
		.slice(0, TAG_LIMIT).map(tag => tag.name)
}


// Takes the recordings of this song, which by ISRC are all of them and by name only those with the same title,
// artist and length, and the first ISRC among them when Deezer had none
function readRecordings(job, answer, isByName) {
	const recordings = (answer?.recordings || []).filter(found => found.score >= MUSICBRAINZ_SCORE && (!isByName
		|| (isSameText(found.title, job.title) && isSameText(found['artist-credit']?.[0]?.name, job.artistName)
			&& found.length && isSameLength(job, found.length / 1000))))
	if (!recordings.length)
		return false
	job.sources.add('MusicBrainz')
	if (!job.info.isrc)
		job.info.isrc = recordings.flatMap(recording => recording.isrcs || [])[0] || ''
	job.info.tags = readTags(recordings.find(recording => readTags(recording).length) || recordings[0])
	job.info.released = earliest(job.info.released, ...recordings.map(recording => recording['first-release-date']))
	job.artistName = recordings[0]['artist-credit']?.[0]?.name || job.artistName
	return true
}


function readFeatures(job, answer) {
	const features = answer?.content?.[0]
	if (!features)
		return
	job.sources.add('ReccoBeats')
	if (!job.info.bpm && features.tempo > 0)
		job.info.bpm = Math.round(features.tempo)
	if (features.key >= 0 && features.key < PITCHES.length)
		job.info.key = PITCHES[features.key] + (features.mode ? ' major' : ' minor')
	if (job.info.gain === null && typeof features.loudness === 'number')
		job.info.gain = Math.round(features.loudness * 10) / 10
	for (const [mood, level] of Object.entries(MOODS)) {
		const value = level(features)
		if (typeof value === 'number' && !Number.isNaN(value))
			job.info[mood] = Math.round(100 * value)
	}
}


// Only an artist with exactly this name and no namesake as likely counts, since a wrong artist's genres would be
// worse than none
function readArtist(job, answer) {
	const artists = (answer?.artists || []).filter(artist => artist.score >= MUSICBRAINZ_SCORE)
	const exact = artists.filter(artist => artist.name.toLowerCase() === job.artistName.toLowerCase())
	const tags = exact.length === 1 ? readTags(exact[0]) : []
	artistTags.set(job.artistName.toLowerCase(), tags)
	if (tags.length)
		job.sources.add('MusicBrainz')
	return tags
}


function quote(text) {
	return '"' + text.replace(/["\\]/g, ' ') + '"'
}


// Runs as a chain: every step reads the answer to the request before it and says which request comes next
function readLastfmTags(answer) {
	const tags = [answer?.toptags?.tag || []].flat()
	return tags.filter(tag => Number(tag.count) >= LASTFM_WEIGHT).map(tag => tag.name.toLowerCase().trim())
		.filter(name => name && !IGNORED_TAGS.has(name)).slice(0, TAG_LIMIT)
}


function lastfmAddress(method, artist, track) {
	return LASTFM + encodeURIComponent(LASTFM_KEY) + '&method=' + method + '&artist=' + encodeURIComponent(artist)
		+ (track ? '&track=' + encodeURIComponent(track) : '')
}


// A failed answer leaves the list unknown, so the next song asks for it again
function readGenreList(text) {
	if (!text)
		return
	genres = new Map()
	for (const line of (text || '').split('\n')) {
		const name = line.trim()
		if (name)
			genres.set(simplify(name), name)
	}
}


// Genres named in the uploader's keywords and description hashtags, and for an upload no music service knows also in its title, where
// "Late 80s Hard Rock Synthwave" names hard rock and synthwave
function readStyles(job, answer) {
	const found = new Set()
	const hashtags = [...(job.description || '').matchAll(/#([\p{L}\p{N}_-]+)/gu)].map(match => match[1])
	for (const keyword of [...(answer?.videoDetails?.keywords || []), ...hashtags]) {
		const genre = genres.get(simplify(keyword.toLowerCase().replace(GENRE_SUFFIX, '')))
		if (genre)
			found.add(genre)
	}
	if (!job.sources.has('Deezer') && !job.sources.has('MusicBrainz')) {
		const words = job.title.toLowerCase().split(/[^\p{L}\p{N}&'-]+/u).filter(Boolean)
		for (let size = 3; size >= 1; size--) {
			for (let start = 0; start + size <= words.length; start++) {
				const genre = genres.get(simplify(words.slice(start, start + size).join(' ')))
				if (genre && (size > 1 || genre.length >= 4))
					found.add(genre)
			}
		}
	}
	if (found.size)
		job.sources.add('YouTube')
	job.info.styles = [...found].slice(0, TAG_LIMIT)
}


// A song from this computer is not on YouTube, so its steps start from the artist and title its file carries
function* steps(job) {
	const isLocal = job.videoId.startsWith('file://')
	if (!isLocal)
		readYouTube(job, yield {url: YOUTUBE, body: JSON.stringify({context: {client: YOUTUBE_CLIENT}, videoId: job.videoId})})
	const query = job.artistName + ' ' + plainTitle(job.title)
	const track = pickDeezer(job, yield {url: DEEZER + 'search?limit=10&q=' + encodeURIComponent(query)})
	if (track) {
		readDeezerTrack(job, yield {url: DEEZER + 'track/' + track.id})
		const albumId = track.album?.id
		if (albumId && !deezerAlbums.has(albumId))
			deezerAlbums.set(albumId, (yield {url: DEEZER + 'album/' + albumId}) || null)
		readDeezerAlbum(job, deezerAlbums.get(albumId))
	}
	const isFound = job.info.isrc && readRecordings(job, yield {url: MUSICBRAINZ + 'recording?fmt=json&limit=10&query=isrc:'
		+ job.info.isrc}, false)
	if (!isFound && job.title.length < 80) {
		const name = 'recording:' + quote(plainTitle(job.title)) + ' AND artist:' + quote(job.artistName)
		readRecordings(job, yield {url: MUSICBRAINZ + 'recording?fmt=json&limit=10&query=' + encodeURIComponent(name)}, true)
	}
	if (job.info.isrc)
		readFeatures(job, yield {url: RECCOBEATS + encodeURIComponent(job.info.isrc)})
	if (!job.info.tags.length && job.artistName) {
		const known = artistTags.get(job.artistName.toLowerCase())
		job.info.artistTags = known ?? readArtist(job, yield {url: MUSICBRAINZ + 'artist?fmt=json&limit=5&query=artist:'
			+ encodeURIComponent(quote(job.artistName))})
	}
	if (!genres)
		readGenreList(yield {url: GENRE_LIST, isText: true})
	if (LASTFM_KEY && genres) {
		job.info.lastfm = readLastfmTags(yield {url: lastfmAddress('track.gettoptags', job.artistName,
			plainTitle(job.title))})
		const artist = job.artistName.toLowerCase()
		if (!job.info.lastfm.length && !lastfmArtists.has(artist))
			lastfmArtists.set(artist, readLastfmTags(yield {url: lastfmAddress('artist.gettoptags', job.artistName)}))
		if (!job.info.lastfm.length)
			job.info.lastfm = lastfmArtists.get(artist) || []
		if (job.info.lastfm.length)
			job.sources.add('Last.fm')
	}
	if (genres && !isLocal)
		readStyles(job, yield {url: YOUTUBE_PLAYER, body: JSON.stringify({context: {client: YOUTUBE_CLIENT}, videoId: job.videoId})})
}


// What the app knows of the song to begin with, from its row in a playlist
function startJob(videoId, title, artist, album, duration) {
	const job = {
		videoId, title, artist, artistName: firstArtist(artist), seconds: readSeconds(duration), sources: new Set(),
		info: {
			title, artist, album, seconds: readSeconds(duration), released: '', uploaded: '', bpm: 0, gain: null,
			isrc: '', explicit: -1, label: '', genres: [], tags: [], artistTags: [], views: 0, channel: '',
			chapters: [], styles: [], lastfm: [], key: '', cover: '', ...Object.fromEntries(Object.keys(MOODS).map(mood => [mood, -1])),
		},
	}
	job.chain = steps(job)
	job.next = job.chain.next()
	return job
}


// The request the job waits for, as {url, body}, or null once the song is done
function nextRequest(job) {
	return job.next.done ? null : job.next.value
}


// Takes the answer to the last request, or an empty one when it failed, and moves on to the next
function takeAnswer(job, text) {
	job.next = job.chain.next(job.next.value?.isText ? text : parse(text))
}


// The finished information, with lists joined by commas and chapters as JSON, ready to store
function finishJob(job) {
	const info = job.info
	return {
		version: VERSION, title: info.title, artist: info.artist, album: info.album, seconds: info.seconds,
		released: info.released, uploaded: info.uploaded, bpm: info.bpm, gain: info.gain ?? '', isrc: info.isrc,
		explicit: info.explicit, label: info.label, genres: info.genres.join(', '),
		tags: (info.tags.length ? info.tags : info.artistTags).join(', '),
		tagSource: info.tags.length ? 'song' : info.artistTags.length ? 'artist' : '', views: info.views,
		channel: info.channel, description: job.description || '', chapters: JSON.stringify(info.chapters), sources: [...job.sources].join(', '),
		key: info.key, cover: info.cover, styles: info.styles.join(', '), lastfm: info.lastfm.join(', '), ...Object.fromEntries(Object.keys(MOODS).map(mood => [mood, info[mood]])),
	}
}


// Last.fm's wiki texts end in a link to read more, and carry links of their own, which are left out
function plainWiki(text) {
	return (text || '').replace(/<a [^>]*>Read more on Last\.fm<\/a>\.?/g, '').replace(/<[^>]+>/g, '')
		.replace(/&amp;/g, '&').replace(/&quot;/g, '"').replace(/&#39;/g, "'").trim()
}


// The answers of track.getInfo, artist.getInfo, track.getSimilar and artist.getSimilar, as plain text for the
// information dialog, with lists one item to a line
function readLastfmDetails(track, artist, similarTracks, similarArtists) {
	const song = parse(track)?.track
	const singer = parse(artist)?.artist
	return {
		url: song?.url || '', listeners: song?.listeners || '', plays: song?.playcount || '',
		yourPlays: song?.userplaycount ?? '', loved: song?.userloved ?? '', wiki: plainWiki(song?.wiki?.summary),
		artistUrl: singer?.url || '', artistListeners: singer?.stats?.listeners || '',
		artistPlays: singer?.stats?.playcount || '', artistYourPlays: singer?.stats?.userplaycount ?? '',
		artistBio: plainWiki(singer?.bio?.summary),
		similarTracks: [parse(similarTracks)?.similartracks?.track || []].flat()
			.map(item => item.artist?.name + ' · ' + item.name).join('\n'),
		similarArtists: [parse(similarArtists)?.similarartists?.artist || []].flat().map(item => item.name).join('\n'),
	}
}


// What artist.getInfo or album.getInfo tell of an artist or album, for the heading of its page
function readLastfmAbout(text) {
	const answer = parse(text)
	const about = answer?.artist ?? answer?.album
	if (!about)
		return null
	const listeners = about.stats?.listeners ?? about.listeners ?? ''
	const plays = about.stats?.playcount ?? about.playcount ?? ''
	const tags = [about.tags?.tag || []].flat().map(tag => tag.name).filter(name => !IGNORED_TAGS.has(name.toLowerCase()))
	return {
		summary: plainWiki((about.bio ?? about.wiki)?.summary).split(/\n\s*\n/)[0].trim(), listeners: String(listeners), plays: String(plays),
		tags: tags.slice(0, 4).join(', '), url: about.url || '',
	}
}


// The loved tracks on one page of user.getLovedTracks, as artist and title after each other, with the page count
function readLovedTracks(text) {
	const answer = parse(text)?.lovedtracks
	if (!answer)
		return null
	const tracks = [answer.track || []].flat().flatMap(track => [track.artist?.name || '', track.name || ''])
	return {tracks: tracks.join('\n'), pages: Number(answer['@attr']?.totalPages) || 1}
}


// Last.fm's similar tracks as artist and title after each other, one to a line, or its similar artists' names
function readSimilarList(text) {
	const answer = parse(text)
	const tracks = [answer?.similartracks?.track || []].flat()
	if (tracks.length)
		return tracks.flatMap(track => [track.artist?.name || '', track.name || '']).join('\n')
	return [answer?.similarartists?.artist || []].flat().map(artist => artist.name).join('\n')
}
