<!doctype html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>{{ title }}</title>
    <link rel="stylesheet" href="{{ css_href }}">
    <link rel="icon" type="image/svg+xml" href="{{ favicon_href }}">
</head>
<body>
    <aside>
    <a class="brand" href="{{ home_href }}">
    <img class="brand-logo" src="{{ logo_href }}" alt="Toi logo">
    <span>Toi Docs</span>
    </a>
    {{ nav_html }}
    </aside>
    <main>{{ body_html }}</main>
</body>
</html>
