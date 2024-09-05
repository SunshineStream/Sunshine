export default async (url, config) => {
    const response = await fetch(url, config);
    // console.log(response);
    if (response.status === 401) {
        const event = new Event("sunshine:session_expire");
        window.dispatchEvent(event);
    }
    return response;
};
