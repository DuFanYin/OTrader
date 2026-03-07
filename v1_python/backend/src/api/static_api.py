"""Static file serving routes."""

from pathlib import Path

from fastapi import APIRouter, HTTPException
from fastapi.responses import HTMLResponse, RedirectResponse

router = APIRouter()

# Frontend directory path
FRONTEND_DIR = Path(__file__).resolve().parents[3] / "frontend"
PAGES_DIR = FRONTEND_DIR / "pages"

# Page mappings for cleaner routing
PAGE_ROUTES: dict[str, str] = {
    "/": "/strategy/strategy-manager.html",
    "/strategy/strategy-manager.html": "strategy-manager.html",
    "/strategy/strategy-holding.html": "strategy-holding.html",
    "/admin/orders-trades.html": "orders-trades.html",
}


def _serve_html_page(page_name: str) -> HTMLResponse:
    """Serve HTML page with proper error handling."""
    page_path = PAGES_DIR / page_name

    if not page_path.exists():
        raise HTTPException(status_code=404, detail=f"Page '{page_name}' not found")

    try:
        content = page_path.read_text(encoding="utf-8")
        return HTMLResponse(content=content)
    except Exception as e:
        raise HTTPException(status_code=500, detail=f"Error reading page '{page_name}': {str(e)}") from e


@router.get("/", response_class=HTMLResponse)
async def root_redirect() -> RedirectResponse:
    """Redirect root to Strategy Manager."""
    return RedirectResponse(url="/strategy/strategy-manager.html", status_code=302)


@router.get("/strategy/strategy-manager.html", response_class=HTMLResponse)
async def serve_strategy_manager() -> HTMLResponse:
    """Serve the Strategy Manager page."""
    return _serve_html_page("strategy-manager.html")


@router.get("/strategy/strategy-holding.html", response_class=HTMLResponse)
async def serve_strategy_holding() -> HTMLResponse:
    """Serve the Strategy Holding page."""
    return _serve_html_page("strategy-holding.html")


@router.get("/admin/orders-trades.html", response_class=HTMLResponse)
async def serve_orders_trades() -> HTMLResponse:
    """Serve the Orders & Trades page."""
    return _serve_html_page("orders-trades.html")


@router.get("/favicon.ico")
def serve_favicon() -> HTMLResponse:
    """Serve favicon to prevent 404 errors."""
    return HTMLResponse(content="", status_code=204)
