<!--
  Copyright (c) 2024 Intel Corporation
 
  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at
 
     http://www.apache.org/licenses/LICENSE-2.0
 
  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
-->

<script lang="ts">
	import VoiceButton from "$lib/assets/chat/svelte/VoiceButton.svelte";
	import stopRecordingIcon from "$lib/assets/chat/svg/StopRecording.svg";
	import { onDestroy } from "svelte";
	import { createEventDispatcher } from "svelte";
	import { MessageRole } from "$lib/shared/constant/Interface";
	import VoiceWave from "$lib/shared/components/talkbot/VoiceWave.svelte";
	// Audio config
	const dispatch = createEventDispatcher();

	let chunks: any[] = [];
	let audioRecorder: MediaRecorder | undefined = undefined;
	let recordOK: boolean = false;
	let isRecording: boolean = false;
	let interval: number;
	let voiceTimer = 0;
	const MAX_AUDIO_TIME: number = 600;

	async function getAudioPermission() {
		if (navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
			await navigator.mediaDevices
				.getUserMedia({ audio: true })
				.then((stream) => {
					audioRecorder = new MediaRecorder(stream);

					audioRecorder.addEventListener("dataavailable", (event) =>
						chunks.push(event.data)
					);

					audioRecorder.addEventListener("stop", () => {
						const blob = new Blob(chunks, { type: "audio/mp3; codecs=opus" });
						chunks = [];
						dispatch("done", blob);
					});
					recordOK = true;
				})
				.catch((error) => {
					console.error(`The following getUserMedia error occurred: ${error}`);
					recordOK = false;
				});
		} else {
			console.log("getUserMedia() is not supported in your browser");
			recordOK = false;
		}
	}

	async function toggleRecording() {
		if (!recordOK) {
			await getAudioPermission();
		}
		isRecording = !isRecording;
		if (isRecording) {
			audioRecorder?.start();
			interval = setInterval(updateTimer, 1000);
		} else {
			audioRecorder?.stop();
			clearInterval(interval);
			voiceTimer = 0;
		}
	}

	function updateTimer() {
		if (voiceTimer++ > MAX_AUDIO_TIME) {
			toggleRecording();
		}
	}

	onDestroy(() => {
		clearInterval(interval);
	});
</script>

<VoiceWave running={isRecording}  />
<!-- Voice button -->
<button type="submit" on:click={toggleRecording} class="mt-2">
	{#if isRecording}
		<img src={stopRecordingIcon} class="h-7 w-7" alt="" />
	{:else}
		<div class="h-7 w-7"><VoiceButton /></div>
	{/if}
</button>

