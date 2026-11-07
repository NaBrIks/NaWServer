import {reactive} from 'vue';
// One session-only credential pair for probes and device commands. Never persisted.
export const commandCredentials=reactive({user:'demo',password:'showcase-only'});
