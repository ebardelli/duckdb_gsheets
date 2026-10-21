<script>
	import '@evidence-dev/tailwind/fonts.css';
	import '../app.css';
	import { EvidenceDefaultLayout } from '@evidence-dev/core-components';
	import { base } from '$app/paths';
	export let data;
</script>

<EvidenceDefaultLayout {data} hideBreadcrumbs logo="{base}/icon-512.png" githubRepo="https://github.com/ebardelli/duckdb_gsheets">
	<slot slot="content" />
</EvidenceDefaultLayout>
